# LAB-5：系统调用流程与用户态虚拟内存管理

## 实验目标

Lab 4 已经完成了第一个用户进程 `proczero`、用户态陷入与返回流程，以及最简单的 `SYS_helloworld` 系统调用。本次实验在此基础上完善系统调用框架和用户地址空间管理，主要目标如下：

- 建立基于系统调用号和跳转表的通用分发流程。
- 实现用户地址空间与内核地址空间之间的数据复制。
- 通过 `brk` 系统调用支持用户堆的扩展与收缩。
- 通过 Load/Store Page Fault 支持用户栈按需向低地址增长。
- 使用带锁的静态节点仓库管理 `mmap_region_t`。
- 实现 mmap 区域的查找、插入、合并、拆分和释放。
- 实现用户页表的深复制与递归销毁，为后续多进程实验做准备。

## 用户地址空间布局

本实验将用户虚拟地址空间划分为代码、堆、mmap、栈以及陷阱切换区域：

```text
高地址
VA_MAX
├── TRAMPOLINE                  共享的用户态/内核态切换代码
├── TRAPFRAME                   每个进程独有的寄存器现场
├── 用户栈                      从高地址向低地址增长
├── MMAP_END
├── mmap 区域                   [MMAP_BEGIN, MMAP_END)
├── MMAP_BEGIN
├── 用户堆                      从低地址向高地址增长
├── USER_BASE + PGSIZE          初始 heap_top
├── 用户代码和数据              USER_BASE 开始的一页
└── 未映射保护页                [0, USER_BASE)
低地址
```

堆不能增长到 `MMAP_BEGIN` 以上，栈不能增长到 `MMAP_END` 以下。mmap 区域独立放置在二者之间，可以创建和释放离散的虚拟地址区间。

## 系统调用框架

### 陷入与分发

用户程序将系统调用号放入 `a7`，最多六个参数放入 `a0` 到 `a5`，然后执行 `ecall`。寄存器由 trampoline 保存到当前进程的 `trapframe` 中。

完整路径如下：

```text
U-mode syscall(...)
  -> ecall
  -> user_vector
  -> trap_user_handler()
  -> syscall()
  -> syscalls[sys_num]()
  -> 返回值写入 trapframe->a0
  -> trap_user_return()
  -> sret
```

`trap_user_handler()` 识别到 8 号异常后先执行：

```c
p->tf->user_to_kern_epc += 4;
syscall();
```

`sepc` 必须越过当前 `ecall` 指令，否则返回用户态后会重复执行同一次系统调用。

系统调用分发表将调用号与内核服务函数解耦：

```c
static uint64 (*syscalls[])(void) = {
    [SYS_copyin]    sys_copyin,
    [SYS_copyout]   sys_copyout,
    [SYS_copyinstr] sys_copyinstr,
    [SYS_brk]       sys_brk,
    [SYS_mmap]      sys_mmap,
    [SYS_munmap]    sys_munmap,
};
```

`arg_uint32()`、`arg_uint64()` 和 `arg_str()` 负责从 `trapframe` 读取参数。服务函数返回的 `uint64` 被写回 `a0`，用户态可以直接取得结果。

## 用户态与内核态的数据迁移

进入内核后 CPU 已切换到内核页表，用户传入的指针仍是用户虚拟地址，不能直接解引用。因此 `uvm_copyin()`、`uvm_copyout()` 和 `uvm_copyin_str()` 都需要手动查询用户页表。

对于任意一个当前地址，单轮复制步骤为：

1. 使用 `ALIGN_DOWN(addr, PGSIZE)` 得到当前虚拟页起点。
2. 使用 `vm_getpte()` 查询叶子 PTE，并检查 `PTE_V`、`PTE_U` 和读写权限。
3. 通过 `PTE_TO_PA()` 得到物理页地址，再加页内偏移。
4. 本轮最多复制到当前页末尾，然后进入下一页。

关键长度计算如下：

```c
uint64 va_page = ALIGN_DOWN(src, PGSIZE);
uint64 offset = src - va_page;
uint32 n = MIN(len, PGSIZE - offset);
```

`copyin` 和 `copyout` 的分页过程对称，但数据方向和权限检查不同：

- `copyin`：用户页复制到内核缓冲区，检查用户页可读。
- `copyout`：内核缓冲区复制到用户页，检查用户页可写。
- `copyin_str`：逐字符复制，遇到 NUL 结束，并支持字符串跨页。

## 堆与栈管理

### 用户堆

`sys_brk(new_heap_top)` 支持查询、保持、扩展和收缩四种操作。只有新的堆顶发生跨页变化时才需要修改页表。

堆扩展时映射：

```text
[ALIGN_UP(old_heap_top), ALIGN_UP(new_heap_top))
```

堆收缩时解除映射：

```text
[ALIGN_UP(new_heap_top), ALIGN_UP(old_heap_top))
```

这里必须向上对齐，因为堆顶表示字节边界。只要堆顶仍位于某一页内部，该页就仍有一部分属于有效堆空间，不能提前释放。

`uvm_heap_grow()` 为新增页面申请用户物理页，并以 `PTE_R | PTE_W | PTE_U` 建立映射；`uvm_heap_ungrow()` 调用 `vm_unmappages(..., true)` 同时清除映射并归还物理页。

### 用户栈

用户栈初始只有一页。当 U-mode 访问尚未映射的栈地址时，会产生：

- 13 号异常：Load Page Fault。
- 15 号异常：Store/AMO Page Fault。

`trap_user_handler()` 将 `stval` 中的故障地址传给 `uvm_ustack_grow()`。函数首先验证：

```text
MMAP_END <= fault_addr < old_stack_bottom
```

之后将故障地址向下对齐，并一次性映射新栈底到旧栈底之间的所有页面：

```c
uint64 new_bottom = ALIGN_DOWN(fault_addr, PGSIZE);

for(uint64 va = new_bottom; va < old_bottom; va += PGSIZE)
    // 申请物理页并建立用户可读写映射
```

这样即使一次访问跨过多个未映射页，也可以直接把栈扩展到目标地址。

## mmap 节点仓库

`mmap_region_t` 描述的是已经分配给进程的虚拟地址区间：

```c
typedef struct mmap_region
{
    uint64 begin;
    uint32 npages;
    struct mmap_region *next;
} mmap_region_t;
```

内核预留 `N_MMAP = 256` 个描述符。每个描述符由 `mmap_region_node_t` 包装：

```c
typedef struct mmap_region_node
{
    mmap_region_t mmap;
    struct mmap_region_node *next;
} mmap_region_node_t;
```

其中 `mmap.next` 用于进程的已分配 mmap 链表，外层 `next` 用于内核空闲仓库，两者职责不同。

`mmap_init()` 将静态数组 `node_list` 串成初始空闲链。申请时从链首取出节点，释放时使用头插法归还：

```c
// alloc
node = list_head.next;
list_head.next = node->next;

// free
node->next = list_head.next;
list_head.next = node;
```

每次申请和释放都由 `list_lk` 自旋锁保护，因此两个 CPU 可以并发使用仓库而不会破坏链表。

需要注意，测试代码中的两个 `N_MMAP / 2` 切分的是 `mmap_list` 指针数组，而不是固定切分 `node_list` 的下标。锁只覆盖一次申请，不覆盖整个 128 次循环，所以两个 CPU 得到的节点下标可能交错。每个 CPU 的申请子序列仍然递增；按原顺序头插归还后，最终输出中对应的子序列递减。

## mmap 与 munmap

### mmap

`sys_mmap(begin, len)` 检查长度和地址的页对齐、mmap 边界以及整数范围，然后调用 `uvm_mmap()`。为了让 `begin == 0` 时的自动选址结果能返回用户态，`uvm_mmap()` 返回最终映射起点。

指定起点时，函数在有序链表中找到插入位置，并检查它与前后节点不重叠。自动选址时，`uvm_mmap_find()` 从 `MMAP_BEGIN` 开始执行 first-fit 扫描，返回第一个足够大的空隙。

插入新节点后，需要分别检查它是否与前驱、后继相邻：

```text
前驱相邻  -> 合并前驱和新节点
后继相邻  -> 合并当前节点和后继
两侧相邻  -> 最终三个区间合并为一个
```

链表整理完成后，再逐页申请物理页并建立 `PTE_R | PTE_W | PTE_U` 映射。

### munmap

`uvm_munmap()` 要求释放范围完整包含在一个已分配节点内。根据释放区间和原节点的关系分为四种情况：

| 情况 | 链表处理 |
| --- | --- |
| 完整覆盖节点 | 从链表删除节点，并归还描述符 |
| 删除节点前部 | 增大 `begin`，减少 `npages` |
| 删除节点后部 | 保持 `begin`，减少 `npages` |
| 删除节点中部 | 原节点保留左段，申请新节点描述右段 |

链表更新后，`vm_unmappages(..., true)` 负责清除对应 PTE 并释放用户物理页。

## 页表复制与销毁

### 深复制

`uvm_copy_pgtbl()` 不复制三级页表页本身的布局，而是按照进程地址空间中实际有效的区域逐页复制：

1. `USER_BASE` 开始的一页代码和数据。
2. `USER_BASE + PGSIZE` 到 `ALIGN_UP(heap_top)` 的堆。
3. mmap 链表描述的所有离散区域。
4. `TRAPFRAME - ustack_npage * PGSIZE` 到 `TRAPFRAME` 的用户栈。

`copy_range()` 对每个源叶子 PTE 执行：

```c
uint64 old_pa = PTE_TO_PA(*pte);
int flags = PTE_FLAGS(*pte);
uint64 new_pa = (uint64)pmem_alloc(false);

memmove((void *)new_pa, (void *)old_pa, PGSIZE);
vm_mappages(new, va, new_pa, PGSIZE, flags);
```

因此新旧页表中的虚拟地址和权限一致，但映射到不同的用户物理页，修改副本不会影响源页表。

`TRAPFRAME` 和 `TRAMPOLINE` 不在该函数中复制。新进程的页表应事先通过 `proc_pgtbl_init()` 建立自己的 trapframe 映射和共享的 trampoline 映射。

### 递归销毁

`destroy_pgtbl(pgtbl, level)` 遍历当前页表的512个 PTE：

- 无效 PTE 直接跳过。
- `R/W/X` 全为0的有效 PTE 指向下一级页表，递归销毁。
- 叶子 PTE 指向用户物理页，归还到 `user_region`。
- 当前层遍历结束后，将页表页归还到 `kern_region`。

正式递归前还需要特殊处理两个高地址映射：

- `TRAPFRAME` 每个进程独有，但它由 `pmem_alloc(true)` 从内核区分配，因此先只解除映射，再通过 `pmem_free(pa, true)` 释放。
- `TRAMPOLINE` 被所有进程共享，只解除映射，不能释放其物理页。

## 测试方法与结果

所有测试均使用以下命令构建并运行：

```bash
make -B build
make run
```

### Test 1：用户态与内核态数据迁移

用户态先通过 `SYS_copyout` 得到内核数组，再通过 `SYS_copyin` 传回内核；字符串通过 `SYS_copyinstr` 复制。输出中的 `1 2 3 4 5` 和 `hello, world` 说明三条复制路径均正确。

![Test 1](picture/test1.png)

### Test 2：用户堆伸缩

测试依次查询堆顶、增长9页、保持不变，再收缩5页。页表输出显示初始堆顶为 `0x2000`，增长后映射到第10号虚拟页，收缩后仅保留第2到第5号堆页。

<table>
  <tr>
    <td><img src="picture/test2(1).png" alt="Test 2 heap grow"></td>
    <td><img src="picture/test2(2).png" alt="Test 2 heap shrink"></td>
  </tr>
</table>

### Test 3：用户栈自动增长

测试中的局部数组跨越多页。第一次写入使栈从1页增长到2页，第二次访问更低地址使栈从2页一次增长到5页；随后内核分别正确读取 `hello` 和 `world`。

![Test 3](picture/test3.png)

### Test 4：mmap 描述符仓库并发

初始化后，空闲链表按 `0 -> 255` 排列。两个 CPU 各申请并归还128个节点，最终仍能遍历到全部256个下标。

本次运行中最终链表包含从 `255` 和 `222` 开始的两条交错递减子序列。这是两个 CPU 在申请阶段交错取得节点的结果，不影响正确性；关键条件是 `0...255` 每个下标恰好出现一次，没有丢失或重复。

<table>
  <tr>
    <td><img src="picture/test4(1).png" alt="Test 4 initial node list"></td>
    <td><img src="picture/test4(2).png" alt="Test 4 concurrent free result"></td>
  </tr>
</table>

### Test 5：mmap 与 munmap

以 `B = MMAP_BEGIN`、`P = PGSIZE` 表示 mmap 区域起点和页大小，测试过程中的已分配区间变化如下：

| 操作 | 操作后的 mmap 链表 |
| --- | --- |
| `mmap(B+4P, 3P)` | `[B+4P, B+7P)` |
| `mmap(B+10P, 2P)` | 上一段及 `[B+10P, B+12P)` |
| `mmap(B+2P, 2P)` | `[B+2P, B+7P)`、`[B+10P, B+12P)` |
| `mmap(B+12P, P)` | 第二段扩展为 `[B+10P, B+13P)` |
| `mmap(B+7P, 3P)` | 两段连接并合并为 `[B+2P, B+13P)` |
| `mmap(B, 2P)` | 合并为 `[B, B+13P)` |
| `mmap(0, 10P)` | first-fit 后合并为 `[B, B+23P)` |
| `munmap(B+10P, 5P)` | `[B, B+10P)`、`[B+15P, B+23P)` |
| `munmap(B, 10P)` | `[B+15P, B+23P)` |
| `munmap(B+17P, 2P)` | `[B+15P, B+17P)`、`[B+19P, B+23P)` |
| 后续四次 `munmap` | 依次缩短并删除剩余区间，最终为空 |

截图同时展示了 mmap 链表和对应页表，能够看到合并时描述符减少、拆分时描述符增加，以及 munmap 后物理页映射消失。

<table>
  <tr>
    <td><img src="picture/test5(1).png" alt="Test 5 step 1"></td>
    <td><img src="picture/test5(2).png" alt="Test 5 step 2"></td>
  </tr>
  <tr>
    <td><img src="picture/test5(3).png" alt="Test 5 step 3"></td>
    <td><img src="picture/test5(4).png" alt="Test 5 step 4"></td>
  </tr>
  <tr>
    <td><img src="picture/test5(5).png" alt="Test 5 step 5"></td>
    <td><img src="picture/test5(6).png" alt="Test 5 step 6"></td>
  </tr>
  <tr>
    <td><img src="picture/test5(7).png" alt="Test 5 step 7"></td>
    <td><img src="picture/test5(8).png" alt="Test 5 step 8"></td>
  </tr>
  <tr>
    <td><img src="picture/test5(9).png" alt="Test 5 step 9"></td>
    <td><img src="picture/test5(10).png" alt="Test 5 step 10"></td>
  </tr>
  <tr>
    <td><img src="picture/test5(11).png" alt="Test 5 final empty list"></td>
    <td></td>
  </tr>
</table>

最终输出：

```text
alloced mmap_space:
empty
```

说明所有 mmap 描述符、页表映射和用户物理页均已正确释放。

### Test 6：页表复制与销毁

该项为自行设计测试。测试临时构造包含以下内容的源页表：

- 一页代码。
- 两页堆。
- 两段 mmap，共三页。
- 三页用户栈。

复制后逐页比较源和副本：

- 虚拟地址和 PTE 权限一致。
- 页面内容一致。
- `old pa` 与 `new pa` 均不相同。

随后修改副本中的代码页，确认源页不变；销毁副本后再次读取源页表，确认源页表仍然有效；最后销毁源页表。测试没有触发 panic，并输出全部检查通过。

![Test 6](picture/test6.png)

## 实验总结

本次实验把 Lab 4 中单一的 `helloworld` 调用扩展成了完整的系统调用分发和参数传递框架，并让 `proczero` 具备了基本的动态地址空间管理能力。

实验中几个关键认识如下：

- 用户指针只能结合用户页表解释，内核不能在切换页表后直接解引用。
- 页表管理必须同时考虑虚拟区间、叶子映射、物理页和中间页表页四个层次。
- 堆顶是字节边界，而物理内存按页管理，因此扩展和收缩需要采用正确的向上对齐边界。
- 自旋锁保证单次仓库操作的原子性，但不会固定两个 CPU 获得节点的连续范围。
- mmap 描述符记录逻辑虚拟区间，真正的可访问性仍由页表映射决定，两者必须同步更新。
- 页表深复制必须重新分配叶子物理页；销毁时必须区分用户页、内核分配的 trapframe 和共享 trampoline。

至此，内核已经具备系统调用参数传递、堆栈增长、离散映射以及地址空间复制和回收能力，为下一实验引入多进程奠定了基础。
