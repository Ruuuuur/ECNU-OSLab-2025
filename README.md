# LAB-7: 文件系统之磁盘管理

## 实验目标

本次实验引入 VirtIO 虚拟磁盘，并建立文件系统最底层的块级管理能力。

完成的功能包括：

- 使用 `mkfs` 构造包含超级块、inode 区域和 data 区域的磁盘镜像。
- 在内核页表中映射 VirtIO MMIO 寄存器，并接通 PLIC 磁盘中断。
- 建立以 4 KB block 为单位的 VirtIO 磁盘读写路径。
- 实现带活跃/非活跃双链表和 LRU 规则的 buffer cache。
- 实现 buffer 物理页的延迟申请与主动回收。
- 读取并校验超级块，获得文件系统磁盘布局。
- 实现 data bitmap 和 inode bitmap 的分配、释放与打印。
- 增加 11 个用于测试 bitmap 和 buffer 的系统调用。

## 磁盘镜像与布局

QEMU 通过以下参数挂载 `disk.img`：

```makefile
QEMUOPTS += -drive file=$(DISKIMG),if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
```

`src/mkfs/mkfs.c` 是运行在宿主 Linux 上的格式化程序。它创建磁盘镜像、计算各区域位置，并将超级块写入磁盘第 0 块。磁盘布局为：

```text
[ superblock | inode bitmap | inode region | data bitmap | data region ]
```

本实验中 `BLOCK_SIZE` 与页大小相同，均为 4096 字节。一个 bitmap block 可以描述：

```text
BIT_PER_BLOCK = 4096 * 8 = 32768 个资源
```

`inode_disk_t` 为 64 字节，因此每个 inode block 可以保存：

```text
INODE_PER_BLOCK = 4096 / 64 = 64 个 inode
```

根据 `N_INODE = 65536` 和 `N_DATA_BLOCK = 1310720`，最终布局如下：

| 区域 | 磁盘块范围 | 块数 | 作用 |
|---|---:|---:|---|
| superblock | 0 | 1 | 保存魔数、块大小和各区域位置 |
| inode bitmap | 1 - 2 | 2 | 记录 65536 个 inode 是否分配 |
| inode region | 3 - 1026 | 1024 | 保存磁盘 inode |
| data bitmap | 1027 - 1066 | 40 | 记录 data block 是否分配 |
| data region | 1067 - 1311786 | 1310720 | 保存文件数据及索引块 |

超级块是其他文件系统操作的定位依据。内核读取后会检查：

```c
assert(sb.magic_num == FS_MAGIC,
    "fs_init: invalid superblock");
assert(sb.block_size == BLOCK_SIZE,
    "fs_init: invalid block size");
```

魔数用于判断镜像是否属于当前文件系统格式，块大小检查则保证内核和 `mkfs` 对磁盘基本单位的理解一致。

## VirtIO 磁盘接入

### MMIO 映射与地址翻译

VirtIO 设备寄存器从 `VIRTIO_BASE = 0x10001000` 开始。启用内核页表后，必须在 `kvm_init()` 中建立可读写的恒等映射：

```c
vm_mappages(
    kernel_pgtbl,
    VIRTIO_BASE,
    VIRTIO_BASE,
    PGSIZE,
    PTE_R | PTE_W
);
```

VirtIO 描述符需要填写物理地址。驱动中的请求头 `buf0` 位于进程内核栈，而内核栈使用 `KSTACK(i)` 虚拟地址，不能直接作为 DMA 地址。因此 `virtio_disk_rw()` 使用：

```c
pte_t *pte = vm_getpte(NULL, addr, false);
disk.desc[idx[0]].addr = (uint64)PTE_TO_PA(*pte) + off;
```

`kernel_pgtbl` 是 `kvm.c` 内部的静态变量，驱动无法直接取得它。本实验约定 `vm_getpte(NULL, ...)` 表示查询内核页表：

```c
if(pgtbl == NULL){
    pgtbl = kernel_pgtbl;
}
```

### PLIC 与磁盘中断

VirtIO 磁盘使用中断号 `VIRTIO_IRQ = 1`。PLIC 初始化时同时设置 UART 和 VirtIO 的优先级，并在每个 hart 上使能两个中断源：

```c
*(uint32 *)(PLIC_PRIORITY(UART_IRQ)) = 1;
*(uint32 *)(PLIC_PRIORITY(VIRTIO_IRQ)) = 1;

*(uint32 *)PLIC_SENABLE(hartid) =
    (1 << UART_IRQ) | (1 << VIRTIO_IRQ);
```

外设中断处理函数根据 `plic_claim()` 返回的 IRQ 分派处理：

```c
if(irq == UART_IRQ){
    uart_intr();
}else if(irq == VIRTIO_IRQ){
    virtio_disk_intr();
}

if(irq)
    plic_complete(irq);
```

一次磁盘请求的睡眠与唤醒路径为：

```text
进程提交 VirtIO 请求
  -> 等待 buffer 完成并进入睡眠
  -> 设备完成 I/O，向 PLIC 发送 IRQ 1
  -> virtio_disk_intr() 确认设备中断
  -> 清除 buffer->disk 并唤醒等待进程
  -> plic_complete() 确认 PLIC 中断完成
```

设备自身的中断确认和 PLIC 的完成确认属于不同层次，两者都不能省略。

## Buffer Cache

### 数据结构与锁

`buffer_t` 将一个磁盘块与一页内存绑定：

```c
typedef struct buffer {
    uint32 block_num;
    uint32 ref;
    sleeplock_t slk;
    uint8 *data;
    bool disk;
} buffer_t;
```

字段由两类锁分别保护：

- 全局自旋锁 `lk_buf_cache` 保护 `block_num`、`ref` 和链表结构。
- 每个 buffer 的睡眠锁 `slk` 保护 `data` 和磁盘 I/O 状态。

`ref` 表示尚未由 `buffer_put()` 归还的引用数，包括当前持有睡眠锁的调用者和正在等待睡眠锁的调用者。它不是严格的进程数量。

buffer 节点被组织为两个带头节点的双向循环链表：

```text
active:   ref > 0，正在使用或有人等待
inactive: ref = 0，可以命中复用、淘汰或释放物理页
```

每个链表中 `head->next` 表示较活跃的一端，`head->prev` 表示最不活跃的一端。

### 初始化与延迟分配

`buffer_init()` 初始化全局锁、两个空循环链表以及全部固定节点。所有节点最初进入非活跃链表：

```c
node->buf.block_num = BLOCK_NUM_UNUSED;
node->buf.ref = 0;
node->buf.data = NULL;
node->buf.disk = false;
```

`data = NULL` 表示节点尚未占用物理页。只有 `buffer_get()` 真正取得非活跃节点时才调用 `pmem_alloc(false)`，避免在初始化时一次性消耗最多 32 MB 物理内存。

### 获取与 LRU 移动

`buffer_get(block_num)` 在持有全局自旋锁时分三种情况处理：

| 情况 | 处理 |
|---|---|
| active hit | `ref++`，移动到 active 的 `head->next`，然后等待睡眠锁 |
| inactive hit | 取得空闲睡眠锁，`ref++`，移动到 active 的 `head->next` |
| miss | 取 inactive 的 `head->prev`，绑定新块，移动到 active 的 `head->prev` |

inactive hit 时，如果 `data == NULL`，说明块号元数据仍然命中，但物理页曾被回收，需要重新申请页面并从磁盘读入。cache miss 即使复用了已有物理页，也必须读取目标块，因为页面里保存的是旧块内容。

磁盘 I/O 在释放 `lk_buf_cache` 后执行。VirtIO 等待过程可能调用 `proc_sleep()`，不能带着全局自旋锁睡眠。

### 归还与物理页回收

`buffer_put()` 先释放睡眠锁，再在全局锁保护下减少引用：

```c
sleeplock_release(&buf->slk);

spinlock_acquire(&lk_buf_cache);
buf->ref--;
if(buf->ref == 0)
    insert_node(node, false, true);
spinlock_release(&lk_buf_cache);
```

该顺序保证了一个重要不变量：

```text
只要 buffer 位于 inactive 链表，它的睡眠锁一定已经释放。
```

`buffer_put()` 不会自动写磁盘，也不会释放 `data`。调用者修改数据后必须先调用 `buffer_write()`。物理页由 `buffer_freemem(count)` 单独回收：函数从 inactive 的最不活跃端向前扫描，释放至多 `count` 个非空 `data`，随后将指针设为 `NULL`。节点及其 `block_num` 仍被保留，因此以后仍可识别块号命中，但需要重新读盘。

## 文件系统初始化

`fs_init()` 首先初始化 buffer cache，然后通过第 0 块读取超级块：

```c
buffer_init();

buffer_t *buf = buffer_get(FS_SB_BLOCK);
memmove(&sb, buf->data, sizeof(sb));
buffer_put(buf);
```

该逻辑不能直接放在 `main()` 中，因为 `buffer_get()` 可能等待磁盘中断并调用 `proc_sleep()`。此时必须已经存在当前进程和可运行的调度器。

因此，文件系统在 `proczero` 第一次进入 `proc_return()` 时初始化：

```c
spinlock_release(&p->lk);

if(p == proczero){
    fs_init();
}
```

必须先释放进程锁，再执行可能睡眠的磁盘 I/O；同时通过 `p == proczero` 保证 buffer cache 和全局超级块只初始化一次。

## Bitmap 资源管理

inode bitmap 和 data bitmap 都以一个 bit 描述一个资源：

```text
bit = 0：资源空闲
bit = 1：资源已经分配或预留
```

bitmap 只描述分配状态，不记录资源属于哪个文件。后续文件系统通过 inode 的 `index[]` 建立文件与 data block 的关系。

### 单块查找与修改

对 bitmap 内第 `index` 个 bit：

```c
uint32 byte = index / BIT_PER_BYTE;
uint32 shift = index % BIT_PER_BYTE;
uint8 mask = (uint8)(1U << shift);
```

`bitmap_search_and_set()` 从前向后查找第一个为 0 的 bit，将其置 1、写回磁盘并返回块内索引。若当前 bitmap block 没有空闲位，则返回 `(uint32)-1`。

`valid_count` 限定当前 bitmap block 中真正有效的 bit 数量。这样即使资源总数不是 `BIT_PER_BLOCK` 的整数倍，也不会把最后一块的填充 bit 当成真实资源。

`bitmap_clear()` 在清零前断言目标 bit 原本为 1，可以检测重复释放。查找、修改和写回期间始终持有 bitmap buffer 的睡眠锁，因此多个进程不会同时分配到同一个 bit。

### 资源编号换算

data bitmap 中的 bit 描述 data region 内的相对位置，而分配接口返回全局磁盘块号：

```text
data block number =
    sb.data_firstblock
    + bitmap block offset * BIT_PER_BLOCK
    + local bit index
```

释放时执行反向换算：

```c
uint32 relative = block_num - sb.data_firstblock;
uint32 bitmap_block_num =
    sb.data_bitmap_firstblock + relative / BIT_PER_BLOCK;
uint32 local_index = relative % BIT_PER_BLOCK;
```

inode 编号本身从 0 开始，因此不需要加减 data region 起点：

```text
inode number = bitmap block offset * BIT_PER_BLOCK + local bit index
```

## 系统调用

本实验新增 11 个系统调用：

| 系统调用 | 作用 |
|---|---|
| `SYS_alloc_block` | 分配 data block |
| `SYS_free_block` | 释放 data block |
| `SYS_alloc_inode` | 分配 inode |
| `SYS_free_inode` | 释放 inode |
| `SYS_show_bitmap` | 输出 data 或 inode bitmap |
| `SYS_get_block` | 获取指定磁盘块的 buffer |
| `SYS_read_block` | 将 `buf->data` 复制到用户空间 |
| `SYS_write_block` | 将用户数据复制到 buffer 并写回磁盘 |
| `SYS_put_block` | 归还 buffer |
| `SYS_show_buffer` | 输出 active/inactive 链表 |
| `SYS_flush_buffer` | 回收非活跃 buffer 的物理页 |

`SYS_get_block` 返回的是内核 `buffer_t` 指针。用户程序不能直接解引用它，只能把它作为不透明句柄传回其他系统调用。该设计用于本实验测试，完整系统应使用受内核校验的描述符代替暴露内核地址。

用户地址只能结合当前进程页表解释。读取和写入系统调用的数据方向分别为：

```text
SYS_read_block:
    kernel buf->data -> user addr_data
    使用 uvm_copyout()

SYS_write_block:
    user addr_data -> kernel buf->data -> disk
    使用 uvm_copyin() + buffer_write()
```

两种操作都要求当前进程仍持有该 buffer 的睡眠锁。

## 测试结果

测试时临时设置：

```c
#define N_BUFFER N_BUFFER_TEST
```

其中 `N_BUFFER_TEST = 8`，便于观察完整 LRU 链表。每组测试使用 `make -B run` 重新生成空白 bitmap 的磁盘镜像。测试结束后已恢复正式配置：

```c
#define N_BUFFER (32 * 512)
```

### 测试 1：超级块读取

**测试目的：** 验证 VirtIO 磁盘是否能够完成初始化和中断驱动读盘，并检查内核能否通过 buffer cache 读取、校验和解析由 `mkfs` 写入的超级块。

**测试过程：** 内核启动后，`proczero` 第一次进入 `proc_return()`，调用 `fs_init()` 初始化 buffer cache，并请求磁盘第 0 块。进程在等待 I/O 时睡眠，磁盘完成请求后通过 PLIC 中断将其唤醒。内核把第 0 块开头复制到全局 `sb`，检查魔数和块大小，输出五段磁盘布局。随后进入用户态并打印 `hello, world!`，用于确认文件系统初始化结束后仍能正常返回用户程序。

核心测试代码：

```c
int main()
{
    syscall(SYS_print_str, "hello, world!\n");
    while(1);
}
```

![测试 1](picture/test1.png)

**结果分析：** 两个 CPU 正常启动，随后内核成功输出超级块中的五段磁盘布局：inode bitmap 为 block 1 至 2，inode region 为 block 3 至 1026，data bitmap 为 block 1027 至 1066，data region 从 block 1067 开始。块大小为 4096 字节、inode 总数为 65536，与 `mkfs` 的计算一致。最后用户态打印 `hello, world!`，说明 VirtIO 初始化、MMIO 映射、PLIC 中断、磁盘睡眠/唤醒、buffer 读盘以及进入用户态的完整路径均已打通。

### 测试 2：Bitmap 分配与释放

**测试目的：** 验证 data bitmap 和 inode bitmap 的首次适配分配、编号换算、分批释放及磁盘持久化是否正确，并检查释放后的资源能否准确恢复为空闲状态。

**测试过程：**

1. 连续申请 20 个 data block，回收 bitmap buffer 的物理页后打印 data bitmap，检查分配结果是否从 data region 起点连续增长。
2. 释放数组偶数下标对应的 10 个 data block，再次回收缓存并打印，检查剩余资源是否恰好为另一半。
3. 释放其余 10 个 data block，确认 data bitmap 重新为空。
4. 连续申请 20 个 inode，检查 inode 编号是否从 0 开始连续分配。
5. 释放全部 inode 并确认 inode bitmap 为空。

每个阶段主动执行 `SYS_flush_buffer`，使相关 buffer 的 `data` 物理页失效；随后的打印必须重新从磁盘读取 bitmap，因此还能同时验证修改是否真正写回磁盘。

核心测试代码：

```c
#define NUM 20

unsigned int block_num[NUM];
unsigned int inode_num[NUM];

for(int i = 0; i < NUM; i++)
    block_num[i] = syscall(SYS_alloc_block);

syscall(SYS_flush_buffer, 8);
syscall(SYS_show_bitmap, 0);

for(int i = 0; i < NUM; i += 2)
    syscall(SYS_free_block, block_num[i]);

syscall(SYS_flush_buffer, 8);
syscall(SYS_show_bitmap, 0);

for(int i = 1; i < NUM; i += 2)
    syscall(SYS_free_block, block_num[i]);

syscall(SYS_flush_buffer, 8);
syscall(SYS_show_bitmap, 0);

for(int i = 0; i < NUM; i++)
    inode_num[i] = syscall(SYS_alloc_inode);

syscall(SYS_flush_buffer, 8);
syscall(SYS_show_bitmap, 1);

for(int i = 0; i < NUM; i++)
    syscall(SYS_free_inode, inode_num[i]);

syscall(SYS_flush_buffer, 8);
syscall(SYS_show_bitmap, 1);
```

![测试 2](picture/test2.png)

**结果分析：** 第一次输出为 data block 1067 至 1086，说明分配器从 data region 起点连续选择前 20 个空闲 bit，并正确换算为全局磁盘块号。释放数组偶数下标后只剩 1068、1070 至 1086，随后释放奇数下标后 bitmap 为空。inode 分配从编号 0 开始得到 0 至 19，全部释放后同样为空。每次打印前回收 buffer 物理页仍能得到正确结果，说明 bitmap 的修改已经通过 `buffer_write()` 持久化到磁盘，而不是只存在于内存缓存。

### 测试 3：Buffer 读写、LRU 与回收

**测试目的：** 验证 buffer cache 的块级读写、磁盘持久化、active/inactive 链表迁移、引用计数、LRU 顺序及非活跃物理页回收逻辑。

**测试过程：**

1. 在 state-1 输出初始化后的缓存状态，观察超级块 buffer 是否已经归还到 inactive 链表。
2. 获取 block 5000，将 `ABCDEFGH` 写入其 buffer 并同步到磁盘，归还后输出 state-2。
3. 释放全部非活跃 buffer 的物理页，再次获取 block 5000 并读入另一个用户数组；比较写入和读出字符串，并输出 state-3，验证数据来自磁盘而不是旧内存副本。
4. 按 5000、5003、5007、5002、5004 的顺序获取五个 buffer 且暂不归还，输出 state-4，观察 active 链表和 `ref = 1`。
5. 依次归还 5007、5000、5004，输出 state-5，检查这三个节点进入 inactive 头部，而 5003、5002 继续保持 active。
6. 调用 `SYS_flush_buffer(3)`，输出 state-6，检查三个最不活跃且持有物理页的 buffer 是否仅将 `data` 释放为 0，同时保留块号和链表位置。

核心测试代码：

```c
#define PGSIZE 4096
#define BLOCK_BASE 5000

char data[PGSIZE], tmp[PGSIZE];
unsigned long long buffer[8];

for(int i = 0; i < 8; i++)
    data[i] = 'A' + i;
data[8] = '\n';
data[9] = '\0';

buffer[0] = syscall(SYS_get_block, BLOCK_BASE);
syscall(SYS_write_block, buffer[0], data);
syscall(SYS_put_block, buffer[0]);

syscall(SYS_flush_buffer, 8);

buffer[0] = syscall(SYS_get_block, BLOCK_BASE);
syscall(SYS_read_block, buffer[0], tmp);
syscall(SYS_put_block, buffer[0]);

buffer[0] = syscall(SYS_get_block, BLOCK_BASE);
buffer[3] = syscall(SYS_get_block, BLOCK_BASE + 3);
buffer[7] = syscall(SYS_get_block, BLOCK_BASE + 7);
buffer[2] = syscall(SYS_get_block, BLOCK_BASE + 2);
buffer[4] = syscall(SYS_get_block, BLOCK_BASE + 4);

syscall(SYS_put_block, buffer[7]);
syscall(SYS_put_block, buffer[0]);
syscall(SYS_put_block, buffer[4]);

syscall(SYS_flush_buffer, 3);
```

![测试 3-1](<picture/test3(1).png>)

![测试 3-2](<picture/test3(2).png>)

![测试 3-3](<picture/test3(3).png>)

**结果分析：**

- state-1 中 active 为空，读取超级块使用的 buffer 7 位于 inactive 头部并绑定 block 0，说明 `fs_init()` 已正确归还超级块 buffer。
- state-2 中 block 5000 绑定到 buffer 6。写入后执行 `buffer_put()`，因此该节点进入 inactive 头部。
- 清空内存缓存后重新读取 block 5000，输出的 `write data` 和 `read data` 都是 `ABCDEFGH`，说明数据已经写入磁盘，并能在 `data == NULL` 时重新申请物理页读回。
- state-4 中 active 顺序为 5000、5003、5007、5002、5004，五个节点的 `ref` 均为 1，与测试中的 GET 顺序及 miss 插入规则一致。
- 归还 block 5007、5000、5004 后，state-5 只保留 5003 和 5002 在 active 链表；inactive 头部依次为 5004、5000、5007，符合 `buffer_put()` 将最新归还节点插入 `head->next` 的规则。
- state-6 中 active 链表保持不变，inactive 中 5004、5000、5007 对应的三个物理页地址变为 0，而块号仍然保留。这验证了 `buffer_freemem(3)` 只回收最不活跃的可释放物理页，不破坏节点和块号元数据。

## 总结

本次实验完成了从“内核只有内存资源”到“能够异步访问持久化块设备”的扩展。VirtIO 驱动负责设备协议，内核页表和 PLIC 提供寄存器访问及中断入口，buffer cache 在磁盘与内存之间提供互斥、缓存和生命周期管理，bitmap 则在其上实现持久化资源分配。

关键结论如下：

- MMIO 地址在启用页表后也必须建立显式映射，设备 DMA 使用物理地址而不是任意内核虚拟地址。
- 磁盘 I/O 可能睡眠，因此不能在初始化主流程或持有自旋锁时等待设备完成。
- buffer 的 `ref` 保护节点生命周期，睡眠锁保护块数据，两者不能互相替代。
- active/inactive 双链表同时表达“是否正在引用”和 LRU 活跃程度，使缓存命中、淘汰与物理页回收可以统一管理。
- bitmap 的 bit 是持久化的资源分配状态；通过 buffer 睡眠锁包围检查、修改和写回，可以避免并发重复分配。
- 用户指针必须通过用户页表和 `uvm_copyin/out()` 解释，不能在内核中直接解引用。

最终代码通过 `make -B build` 完整构建，并通过超级块读取、bitmap 分配释放以及 buffer 持久化/LRU/回收三组测试。
