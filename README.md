# LAB-8：文件系统之数据组织与层次结构

## 实验目标

Lab 7 已经实现了以 block 为单位访问 VirtIO 磁盘、管理 bitmap 和维护 buffer cache。本次实验在此基础上完成文件系统中更高层的数据组织：

- 使用 `mkfs` 创建带有根目录和初始目录项的磁盘镜像。
- 使用 inode 描述文件类型、文件大小以及文件数据对应的磁盘块。
- 支持直接索引、一级间接索引和二级间接索引。
- 实现 inode 的磁盘副本、内存缓存、引用计数和生命周期管理。
- 使用 dentry 建立文件名到 inode 编号的映射。
- 实现目录项的查找、创建、删除以及绝对路径解析。
- 验证文件数据读写、目录层次组织和路径到 inode 的转换。

## 磁盘镜像与文件系统布局

`src/mkfs/mkfs.c` 在宿主机上运行，负责生成 QEMU 使用的 `target/mkfs/disk.img`。镜像按照固定区域组织：

```text
[ super block | inode bitmap | inode region | data bitmap | data region ]
```

本实验中 block 大小为 4096 字节，磁盘布局如下：

| 区域 | 块号 | 作用 |
| --- | ---: | --- |
| super block | 0 | 保存魔数、块大小及各区域起始位置 |
| inode bitmap | 1 - 2 | 记录 inode 是否已分配 |
| inode region | 3 - 1026 | 保存磁盘上的 inode |
| data bitmap | 1027 - 1066 | 记录 data block 是否已分配 |
| data region | 1067 - 1311786 | 保存文件数据和索引块 |

`mkfs` 首先保留 inode 0 作为根目录，然后为根目录写入四个目录项：

```text
.          -> inode 0
..         -> inode 0
ABCD.txt   -> inode 1
abcd.txt   -> inode 2
```

根目录的数据块是 data region 的第一个块，即块 1067。目录项大小为 64 字节，因此一个 4096 字节的数据块最多容纳 64 个目录项。

内核启动后，`fs_init()` 通过 `buffer_get(FS_SB_BLOCK)` 读取超级块，并检查魔数和块大小：

```c
buffer_init();

buffer_t *buf = buffer_get(FS_SB_BLOCK);
memmove(&sb, buf->data, sizeof(sb));
buffer_put(buf);

assert(sb.magic_num == FS_MAGIC,
    "fs_init: invalid superblock");
assert(sb.block_size == BLOCK_SIZE,
    "fs_init: invalid block size");
```

文件系统初始化必须在 `proczero` 已经建立之后进行，因为 `buffer_get()` 可能等待 VirtIO 磁盘中断并使当前进程睡眠。

## inode：文件数据的组织

### 磁盘 inode 与内存 inode

磁盘上的 `inode_disk_t` 只保存持久化信息：

```c
typedef struct inode_disk {
    short type;
    short major;
    short minor;
    short nlink;
    unsigned int size;
    unsigned int index[INODE_INDEX_3];
} inode_disk_t;
```

内存中的 `inode_t` 在此基础上增加了缓存和并发控制信息：

```c
typedef struct inode {
    inode_disk_t disk_info;
    bool valid_info;
    uint32 inode_num;
    uint32 ref;
    sleeplock_t slk;
} inode_t;
```

- `valid_info` 表示内存中的 `disk_info` 是否已经从磁盘读入。
- `inode_num` 用于根据 inode 编号定位 inode region 中的磁盘位置。
- `ref` 是 inode cache 的引用计数，不是文件链接数。
- `slk` 保护 `disk_info` 和 `valid_info`，磁盘 I/O 期间允许进程睡眠。
- `nlink` 是磁盘 inode 中的链接数，决定 inode 是否仍然存在；它和内存缓存的 `ref` 是不同概念。

`inode_rw(ip, false)` 将 inode region 中的磁盘 inode 读入内存，`inode_rw(ip, true)` 则把内存中的元数据写回磁盘。两种操作都要求调用者持有 inode 睡眠锁。

### 直接索引和间接索引

`index[]` 按三层方式组织文件数据：

```text
index[0..9]       直接索引       -> 数据块
index[10..11]     一级间接索引   -> 一级索引块 -> 数据块
index[12]         二级间接索引   -> 二级索引块 -> 一级索引块 -> 数据块
```

一个索引块可以保存 `4096 / 4 = 1024` 个块号，所以：

- 直接索引覆盖 10 个 block，即 40 KB。
- 一级间接索引覆盖 `2 * 1024` 个 block，即 8 MB。
- 二级间接索引覆盖 `1024 * 1024` 个 block，即约 4 GB。

逻辑块号和物理块号不是同一个概念。逻辑块号是文件内从 0 开始的偏移，物理块号是磁盘 data region 中的实际块号。`locate_or_add_block()` 根据逻辑块号选择直接、一级间接或二级间接路径；如果路径中缺少索引块或数据块，就调用 `alloc_zero_block()` 分配并清零新的 data block。

分配索引块时也必须将索引块本身写回磁盘。例如，当文件从直接索引区域扩展到一级间接区域时，需要依次完成：

```text
分配一级索引块
  -> 将一级索引块号写入 inode.index[10]
  -> 分配真正的数据块
  -> 将数据块号写入一级索引块
  -> 写回 inode 和索引块
```

`free_data_blocks()` 使用递归方式释放索引树。递归到叶子时释放数据块，返回上一层后再释放索引块，因此不会遗留只保存块号的中间索引页。

### 数据读写

`inode_read_data()` 和 `inode_write_data()` 都以 buffer 作为磁盘和内存之间的中间载体。每轮处理一个 block 内的一段数据：

```c
position      = offset + copied;
logical_block = position / BLOCK_SIZE;
block_offset  = position % BLOCK_SIZE;
cut_len       = MIN(len - copied,
                    BLOCK_SIZE - block_offset);
```

因此即使源地址、目标地址或文件偏移不是 page-aligned，也可以正确处理跨页和跨 block 的读写。写入时，函数通过 `locate_or_add_block()` 确保目标逻辑块存在，修改 buffer 后调用 `buffer_write()`；成功写入后更新 inode 的 `size` 并通过 `inode_rw()` 持久化元数据。

对 `INODE_TYPE_DATA`，文件不允许出现空洞，`size` 表示 `[0, size)` 范围内已经有效的数据量。对 `INODE_TYPE_DIR`，`size` 表示已经使用的目录项空间，目录项删除后可以留下空槽位供后续复用。

## inode 生命周期与缓存

inode cache 的固定数组由 `inode_init()` 初始化。`inode_get()` 首先查找相同 inode 编号的缓存项，命中时只增加 `ref`；未命中时选择 `ref == 0` 的空槽位，设置 inode 编号并从磁盘读取元数据。

典型生命周期如下：

```text
inode_create
  -> bitmap_alloc_inode 分配 inode 编号
  -> 初始化内存 inode
  -> inode_rw 写入 inode region

inode_get / inode_dup
  -> 增加内存缓存引用

inode_lock / inode_unlock
  -> 获取或释放 inode 睡眠锁

inode_put
  -> ref--
  -> 当 ref == 0 且 nlink == 0 时调用 inode_delete
  -> 释放 data block、索引块和 inode bitmap 位
```

`inode_dup()` 只复制一个内存引用，不复制磁盘 inode 和数据块。`inode_delete()` 由最后一个引用触发，负责释放该 inode 管理的全部数据资源。这样可以把“当前有多少内核代码正在使用 inode”和“文件是否仍被目录项链接”分开处理。

## dentry：文件名到 inode 的映射

目录本身也是 inode，区别在于它的数据块被解释为连续的 `dentry_t` 数组：

```c
typedef struct dentry {
    char name[MAXLEN_FILENAME];
    unsigned int inode_num;
} dentry_t;
```

### 查找、创建和删除

`dentry_search()` 在目录数据块中遍历有效槽位，通过 `name` 查找 inode 编号。`name[0] == '\0'` 表示该槽位空闲。

`dentry_create()` 的流程为：

1. 检查文件名、inode 编号、目录类型和目录容量。
2. 如果目录尚未分配 `index[0]`，从 data bitmap 分配目录数据块并清零。
3. 扫描所有槽位，同时检查重名并记录第一个空槽位。
4. 写入文件名和 inode 编号。
5. 调用 `buffer_write()` 持久化目录块，增加目录 inode 的 `size` 并写回 inode。

`dentry_delete()` 清零匹配的目录项，使其重新成为空槽位，同时减少目录 inode 的 `size`。本实验不允许删除 `.` 和 `..`。删除目录项不会自动删除目标 inode，因为 inode 可能仍被其他引用或链接使用。

目录项操作要求调用者先持有父目录的睡眠锁。否则两个 CPU 可能同时找到同一个空槽位，导致其中一次写入覆盖另一次，或者同时修改目录大小造成元数据不一致。

## 路径解析

`get_element()` 每次跳过连续的 `/`，从绝对路径中提取一个文件名。`__path_to_inode()` 以根 inode 0 为起点，循环执行：

```text
当前 inode
  -> inode_lock
  -> dentry_search(name)
  -> 得到下一级 inode_num
  -> inode_get(inode_num)
  -> 释放当前 inode 引用
```

因此路径 `///AABBC///aaabb/file.txt` 的解析过程为：

```text
ROOT_INODE(0)
  -> AABBC  -> inode 3
  -> aaabb  -> inode 4
  -> file.txt -> inode 5
```

`path_to_inode()` 返回最后一个路径元素对应的 inode；`path_to_parent_inode()` 返回最后一个元素的父目录，并通过 `name` 参数返回最后一级名称。连续斜杠会被 `get_element()` 当作分隔符跳过，因此不会影响路径解析。

## 测试方法

每次切换测试用例后先退出旧的 QEMU，再执行：

```bash
make -B run
```

`disk.img` 是持久化文件，普通的 `make run` 可能继续使用上一轮测试留下的目录项或 bitmap 状态。`make -B run` 会强制重新编译并重新运行 `mkfs`，得到干净的初始镜像。本次测试结束后，`fs.c` 中的临时测试代码和诊断输出已经移除，恢复为只负责初始化文件系统的提交状态。

## 测试结果

### 测试 1：inode 的访问、创建、引用和删除

**测试目的：** 验证根 inode 的读取、inode 创建、`ref` 引用计数、链接数和 inode bitmap 的变化。

核心测试逻辑如下：

```c
rooti = inode_get(ROOT_INODE);
inode_lock(rooti);
inode_print(rooti, "root");
inode_unlock(rooti);
bitmap_print(false);

ip_1 = inode_create(INODE_TYPE_DIR,
    INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
ip_2 = inode_create(INODE_TYPE_DATA,
    INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
inode_lock(ip_1);
inode_lock(ip_2);
inode_dup(ip_2);

inode_print(ip_1, "dir");
inode_print(ip_2, "data");
bitmap_print(false);

ip_1->disk_info.nlink = 0;
ip_2->disk_info.nlink = 0;
inode_unlock(ip_1);
inode_unlock(ip_2);
inode_put(ip_1);
inode_put(ip_2);
bitmap_print(false);
inode_put(ip_2);
bitmap_print(false);
```

![测试 1](picture/test1.png)

**结果分析：** 初始 bitmap 为 `0 1 2`，表示根 inode 和 mkfs 预置的两个文件已经占用编号 0、1、2。创建目录和数据 inode 后，bitmap 变为 `0 1 2 3 4`。`inode_dup(ip_2)` 使数据 inode 的内存引用数变为 2；第一次 `inode_put(ip_2)` 只减少引用，不释放 inode。目录 inode 的引用归零且 `nlink == 0` 后被释放，bitmap 变为 `0 1 2 4`。最后一次归还数据 inode 后，bitmap 恢复为 `0 1 2`。这说明 inode 的引用计数、链接数和资源回收条件工作正确。

### 测试 2：小文件、大文件和多级索引读写

**测试目的：** 验证跨 block 的读写、直接索引、一级间接索引和二级间接索引的按需创建，以及数据块和索引块的回收。

小文件测试以 10 个整数为一组重复写入，并从非对齐位置读取：

```c
int small_src[10], small_dst[10];
for (int i = 0; i < 10; i++)
    small_src[i] = i;

cut_len = 10 * sizeof(int);
for (uint32 offset = 0;
     offset < 400 * cut_len;
     offset += cut_len) {
    len = inode_write_data(ip_1, offset,
        cut_len, small_src, false);
    assert(len == cut_len, "write fail 1!");
}

len = inode_read_data(ip_1,
    120 * cut_len + 4, cut_len,
    small_dst, false);
```

大文件测试使用 4 页加 1110 字节作为单次写入长度，重复写入到超过直接索引和一级间接索引范围的位置，再从文件末尾读取数据：

```c
cut_len = PGSIZE * 4 + 1110;
for (uint32 offset = 0;
     offset < cut_len * 10000;
     offset += cut_len) {
    len = inode_write_data(ip_2, offset,
        cut_len, big_src, false);
    assert(len == cut_len, "write fail 2!");
}

len = inode_read_data(ip_2,
    cut_len * 10000 - 8, 8,
    big_dst, false);
```

![测试 2](picture/test2.png)

**结果分析：** 小文件最终大小为 16000 字节，直接索引中出现 4 个数据块 `1074` 至 `1077`，从偏移 `120 * 40 + 4` 读取出的整数为 `1 2 3 4 5 6 7 8 9 0`，说明非 block 对齐的读取正确。大文件最终大小为 `174940000` 字节，输出同时出现直接索引、一级索引块和二级索引块：

```text
direct:  1074 ... 1083
level-1: 1084, 2109
level-2: 3134
```

这说明文件扩展过程中，索引树能够按需增长，并且数据写入已经跨越了直接和一级间接区域。末尾读取到 `GHABCDEF`，与测试写入的字节模式一致，证明二级索引路径也能正确定位数据块。

### 测试 3：目录项的查找、创建和删除

**测试目的：** 验证 mkfs 预置目录项的读取，以及新目录项的分配、重名检查、槽位偏移和删除复用逻辑。

核心操作如下：

```c
inode_lock(rooti);
inode_num_1 = dentry_search(rooti, "ABCD.txt");
inode_num_2 = dentry_search(rooti, "abcd.txt");
inode_num_3 = dentry_search(rooti, ".");
dentry_print(rooti);
inode_unlock(rooti);

inode_lock(rooti);
ip_1 = inode_create(INODE_TYPE_DIR,
    INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
offset = dentry_create(rooti,
    ip_1->inode_num, "new_dir");
inode_num_1 = dentry_search(rooti, "new_dir");

inode_num_2 = dentry_delete(rooti, "new_dir");
assert(inode_num_1 == inode_num_2,
    "inode num is not equal!");
inode_unlock(rooti);
```

![测试 3](picture/test3.png)

**结果分析：** 初始目录包含 `.`、`..`、`ABCD.txt` 和 `abcd.txt`，其目录项偏移分别为 0、64、128 和 192。新目录项被放在第一个空槽位，返回偏移 256，并记录 inode 3；随后 `dentry_search()` 找到同一个 inode 编号。删除后，`new_dir` 不再出现在 `dentry_print()` 输出中，原有四个目录项仍然完整。结果说明目录项内容和目录 inode 元数据都已写回磁盘，删除操作只清空目录槽位，不会错误删除目标 inode 本身。

### 测试 4：多级目录和绝对路径解析

**测试目的：** 验证目录项和 inode 的组合使用，以及包含连续斜杠的多级绝对路径解析。

核心测试逻辑如下：

```c
rooti = inode_get(ROOT_INODE);
ip_1 = inode_create(INODE_TYPE_DIR,
    INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
ip_2 = inode_create(INODE_TYPE_DIR,
    INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
ip_3 = inode_create(INODE_TYPE_DATA,
    INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);

inode_lock(rooti);
inode_lock(ip_1);
inode_lock(ip_2);
inode_lock(ip_3);

assert(dentry_create(rooti,
    ip_1->inode_num, "AABBC") != (uint32)-1);
assert(dentry_create(ip_1,
    ip_2->inode_num, "aaabb") != (uint32)-1);
assert(dentry_create(ip_2,
    ip_3->inode_num, "file.txt") != (uint32)-1);

char tmp1[] = "This is file context!";
inode_write_data(ip_3, 0,
    sizeof(tmp1), tmp1, false);

char *path = "///AABBC///aaabb/file.txt";
char name[MAXLEN_FILENAME];
ip_4 = path_to_inode(path);
ip_5 = path_to_parent_inode(path, name);
```

![测试 4](picture/test4.png)

**结果分析：** 根目录初始大小为 256 字节，`AABBC` 被插入到空闲槽位；之后依次创建 `aaabb` 和 `file.txt`。`get a name = file.txt` 说明 `path_to_parent_inode()` 正确提取最后一级名称。`path_to_inode()` 返回的文件 inode 编号为 5，文件大小为 22 字节，数据块为 1076；父目录 `aaabb` 的 inode 编号为 4，目录大小为 64 字节，数据块为 1075。最后读回的字符串与写入内容完全一致，说明路径中的连续斜杠被正确跳过，并且每一级目录都经过了正确的 inode 和 dentry 查找。

## 调试与问题处理

### 测试镜像的持久化

`disk.img` 不是每次普通 `make run` 都会重新生成。重复运行目录项测试时，如果上一次运行已经写入 `AABBC`，下一次 `dentry_create()` 会因为重名返回失败，这是正确的保护行为，不是 dentry 实现错误。测试时退出旧 QEMU 后执行 `make -B run`，确保内核、用户程序和磁盘镜像都重新构建。

### 连续物理页测试

测试 2 需要在初始化阶段申请连续的物理页。物理页空闲链表采用反向建立方式，使连续的 `pmem_alloc(true)` 返回递增的物理地址，满足测试对连续页的检查。该修改只影响物理页空闲链表的初始顺序，不改变页的分配和释放语义。

### 内核时钟中断后的 CSR 恢复

Lab 6 的内核态时钟抢占会调用 `proc_yield()`。为避免切换返回后使用错误的陷阱寄存器状态，`trap_kernel_handler()` 在处理结束时恢复保存的 `sepc` 和 `sstatus`。这是文件系统测试能够稳定运行的前置模块修复。

## 总结

本次实验完成了从“磁盘块”到“文件”和“路径”的两层抽象：

- inode 用索引树把一个文件的逻辑块连续空间映射到离散的物理 data block，并负责文件元数据和资源生命周期。
- dentry 用目录项把人类可读的名称映射为 inode 编号，目录 inode 由此形成层次化的文件命名空间。
- buffer cache 是 inode、目录和磁盘之间的共享中间层，所有修改都需要通过 `buffer_write()` 和 `inode_rw()` 持久化。
- inode 的 `ref`、`nlink` 和锁分别解决缓存共享、文件存在性和并发访问问题。
- 路径解析本质上是重复执行“当前目录 inode -> dentry -> 下一级 inode”的过程。

四组测试均正常到达 `test end`，验证了 inode 创建与回收、直接和多级间接索引、数据读写、目录项管理以及多级绝对路径解析。完成本实验后，文件系统已经具备了后续实现普通文件、目录文件和设备文件接口所需的底层数据组织能力。
