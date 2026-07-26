# LAB-6: 单进程走向多进程

## 实验目标

本次实验在 Lab 5 的用户地址空间和系统调用基础上，引入进程表、进程调度和进程生命周期管理，使内核从只能运行 `proczero` 扩展为能够并发管理多个用户进程。

完成的功能包括：

- 建立带 PID、状态和进程锁的 `proc_list[N_PROC]` 进程仓库。
- 为每个进程槽位配置独立内核栈，并实现循环扫描调度器。
- 在用户态和内核态时钟中断后执行抢占式让出 CPU。
- 实现 `fork`、`exit`、`wait`、父子关系和孤儿进程过继。
- 实现基于等待通道的 `sleep/wakeup`、睡眠锁以及按 tick 计时的 `sleep` 系统调用。
- 增加进程相关的系统调用：`print_str`、`print_int`、`getpid`、`fork`、`wait`、`exit` 和 `sleep`。

## 进程表与初始化

`proc_list` 是固定大小的进程仓库。每个 `proc_t` 除了用户页表、trapframe、堆顶和 mmap 链表外，还维护如下共享状态：

- `lk`：保护 `state`、`parent`、`exit_code` 和 `sleep_space`。
- `state`：`UNUSED`、`RUNNABLE`、`RUNNING`、`SLEEPING`、`ZOMBIE`。
- `parent`：父进程，用于 `wait` 回收和孤儿进程过继。
- `sleep_space`：等待通道，用于区分不同的睡眠原因。

`proc_init()` 初始化 PID 锁和每个进程槽位。内核栈虚拟地址由槽位编号固定决定：

```c
p->kstack = KSTACK(i);
```

在 `kvm_init()` 中循环为全部 `N_PROC` 个槽位分配并映射内核栈物理页。`KSTACK(i)` 之间保留未映射保护页，因此内核栈越界会触发异常，而不会覆盖相邻进程的内核栈。

`proc_alloc()` 在持有候选进程锁的情况下申请 `UNUSED` 槽位，并初始化 PID、trapframe、用户页表和内核态上下文：

```c
p->ctx.ra = (uint64)proc_return;
p->ctx.sp = p->kstack + PGSIZE;
```

第一次调度到新进程后，`swtch()` 从其 `ctx.ra` 进入 `proc_return()`。该函数释放跨上下文切换传递的进程锁，再调用 `trap_user_return()` 进入用户态。

`proc_make_first()` 使用上述通用路径建立 `proczero`，映射 initcode 和初始用户栈，设置用户 PC 为 `USER_BASE`，最后将状态设为 `RUNNABLE` 交给调度器。

## 调度与抢占

每个 CPU 在初始化完成后进入 `proc_scheduler()`。调度器循环扫描进程表，选择 `RUNNABLE` 进程并执行：

```c
p->state = RUNNING;
cpu->proc = p;
swtch(&cpu->ctx, &p->ctx);
```

进程让出 CPU 时，`proc_yield()` 将状态从 `RUNNING` 改为 `RUNNABLE`，然后通过 `proc_sched()` 执行反向切换：

```c
swtch(&p->ctx, &cpu->ctx);
```

两次切换之间，进程锁不被提前释放。调度器将进程锁持有到切换后；第一次进入进程时由 `proc_return()` 释放，后续从 `proc_sched()` 返回时由让出 CPU 的进程释放。这保证了状态变化和实际执行流切换不会被其他 CPU 观察到不一致的中间状态。

用户态和内核态时钟中断处理结束后都会调用 `proc_yield()`。内核态中断还会先检查 `myproc() != NULL`，避免调度器自身收到时钟中断时错误让出不存在的当前进程。这样每个可运行进程得到一个长度为一个时钟周期的时间片。

## `fork`、`exit` 与 `wait`

### 进程复制

`proc_fork()` 通过 `proc_alloc()` 取得新的进程槽位后，对父进程进行深复制：

- 复制 trapframe，使子进程从同一条系统调用后的用户指令继续执行。
- 调用 `uvm_copy_pgtbl()` 复制代码、堆、mmap 区和用户栈对应的物理页。
- 深复制 mmap 描述符链表，避免父子进程共享同一组链表节点。
- 复制堆顶、用户栈页数、名称并建立 `child->parent`。

父子进程保留相同用户虚拟地址布局，但其普通用户页指向不同物理页，因此任一方修改堆、栈或 mmap 区域都不会修改另一方。

子进程通过：

```c
child->tf->a0 = 0;
```

获得 `fork` 返回值 `0`；父进程从 `proc_fork()` 得到新 PID，系统调用框架再将该 PID 写回父进程的 `a0`。

### 退出和回收

进程不能在仍使用自身内核栈和上下文时释放自己。因此 `proc_exit()` 不直接调用 `proc_free()`，而是执行如下流程：

1. 通过 `proc_reparent()` 将自己的孩子过继给永不退出的 `proczero`。
2. 按“父锁再子锁”的顺序取得锁，记录 `exit_code` 并将自身设为 `ZOMBIE`。
3. 唤醒正在等待自己的父进程。
4. 持有自身进程锁切入调度器，此后不再被调度。

`proc_wait()` 在持有父进程锁时扫描子进程。若发现 `ZOMBIE`，它先用 `uvm_copyout()` 将退出码写到父进程提供的用户地址，再调用 `proc_free()` 释放该子进程的页表、用户页、trapframe 和 mmap 描述符，最后返回子 PID。若没有子进程则返回 `-1`；若有子进程但暂未退出，则以父进程自身为等待通道进入睡眠。

## 睡眠、唤醒和睡眠锁

`proc_sleep(sleep_space, lock)` 用于等待外部资源。调用者持有资源锁并确认条件尚未满足后，函数先取得当前进程锁，再释放资源锁，最后写入等待通道并进入 `SLEEPING`：

```text
持有资源锁
  -> 取得当前进程锁
  -> 释放资源锁
  -> 设置 sleep_space 和 SLEEPING
  -> proc_sched()
```

这种锁交接既避免进程睡眠时长期占有资源锁，也避免“条件已变化但进程尚未标记为睡眠”导致的丢失唤醒。`proc_wakeup()` 扫描进程表，将等待同一 `sleep_space` 的全部进程从 `SLEEPING` 改为 `RUNNABLE`；被唤醒的进程仍需重新检查条件。

睡眠锁 `sleeplock_t` 使用内部自旋锁保护 `locked` 和 `pid`。获取失败的进程以睡眠锁对象地址作为等待通道睡眠：

```c
while(lk->locked){
    proc_sleep(lk, &lk->lock);
}
```

释放睡眠锁时清除持有者信息，再唤醒所有等待该睡眠锁的进程。使用 `while` 而非 `if`，使多个被同时唤醒的进程能够重新竞争锁并再次检查条件。

## 时钟等待与系统调用

系统时钟 `sys_timer` 的 `ticks` 字段由 CPU 0 在时钟中断中递增。每次更新后调用：

```c
proc_wakeup(&sys_timer);
```

`timer_wait(ntick)` 在持有 `sys_timer.lk` 时记录起始 tick，并循环检查经过的 tick 数；未达到目标时以 `&sys_timer` 为等待通道睡眠。`sys_sleep()` 从 `a0` 读取等待时长并调用 `timer_wait()`。

新增系统调用均通过现有的 `a7` 系统调用号和 trapframe 参数通道进入内核：

- `sys_print_str()` 使用 `arg_str()` 和 `uvm_copyin_str()` 将用户字符串复制到内核缓冲区。
- `sys_print_int()` 输出 32 位整数。
- `sys_getpid()` 返回当前进程 PID。
- `sys_fork()`、`sys_wait()`、`sys_exit()`、`sys_sleep()` 分别转发给进程和时钟模块。

## 测试结果


### 测试 1：PID 与字符串输出

测试 `SYS_getpid`、字符串参数复制和 `SYS_print_str`。仅 PID 为 1 的 `proczero` 输出欢迎信息。


```c
int pid = syscall(SYS_getpid);

if(pid == 1){
    syscall(SYS_print_str, "\nproczero: hello ");
    syscall(SYS_print_str, "world!\n");
}

while(1)
    ;
```

![测试 1](picture/test1.png)

**结果分析：** 两个 CPU 均完成启动，且仅 PID 为 1 的 `proczero` 输出欢迎信息。这说明 `sys_getpid()` 能正确取得当前进程，用户字符串地址能够经 `arg_str()` 和 `uvm_copyin_str()` 按当前用户页表复制到内核，再由 `sys_print_str()` 输出。

### 测试 2：两次 `fork`

用户程序在两次 `fork` 前后分别输出三个层级。输出次数应为：`level-1!` 一次、`level-2!` 两次、`level-3!` 四次。调度顺序可变化，但进程 PID 最终扩展到 1 至 4。


```c
syscall(SYS_print_str, "level-1!\n");
syscall(SYS_fork);
syscall(SYS_print_str, "level-2!\n");
syscall(SYS_fork);
syscall(SYS_print_str, "level-3!\n");

while(1)
    ;
```

![测试 2](picture/test2.png)

**结果分析：** 截图中 `level-1!`、`level-2!`、`level-3!` 分别出现 1、2、4 次，进程 PID 从 1 扩展到 4。每个进程在第二次 `fork` 后都拥有独立的后续执行流，因此输出数量按 2 倍增长。不同 PID 的运行顺序交错是双 CPU 与时钟抢占调度的正常结果，不影响 fork 的正确性。

### 测试 3：地址空间复制、退出与回收

父进程先创建 mmap 区、堆区和用户栈字符串，再执行 `fork`。子进程能够分别读出 `MMAP_REGION`、`HEAP_REGION` 和 `STACK_REGION`，说明用户页和 mmap 元数据复制正确；子进程以退出码 1234 退出，父进程 `wait` 后得到 PID 2 和正确退出状态。


```c
#define PGSIZE 4096
#define VA_MAX (1ul << 38)
#define MMAP_END (VA_MAX - (2 + 16 * 256) * PGSIZE)
#define MMAP_BEGIN (MMAP_END - 64 * 256 * PGSIZE)

int pid, i;
char *str1, *str2, *str3 = "STACK_REGION\n\n";
char *tmp1 = "MMAP_REGION\n", *tmp2 = "HEAP_REGION\n";

str1 = (char *)syscall(SYS_mmap, MMAP_BEGIN, PGSIZE);
for(i = 0; tmp1[i] != '\0'; ++i)
    str1[i] = tmp1[i];
str1[i] = '\0';

str2 = (char *)syscall(SYS_brk, 0);
syscall(SYS_brk, (long long int)str2 + PGSIZE);
for(i = 0; tmp2[i] != '\0'; ++i)
    str2[i] = tmp2[i];
str2[i] = '\0';

syscall(SYS_print_str, "\n--------test begin--------\n");
pid = syscall(SYS_fork);
if(pid == 0){
    syscall(SYS_print_str, "child proc: hello!\n");
    syscall(SYS_print_str, str1);
    syscall(SYS_print_str, str2);
    syscall(SYS_print_str, str3);
    syscall(SYS_exit, 1234);
}else{
    int exit_state = 0;
    syscall(SYS_wait, &exit_state);
    syscall(SYS_print_str, "parent proc: hello!\n");
    syscall(SYS_print_int, pid);
    if(exit_state == 1234)
        syscall(SYS_print_str, "good boy!\n");
}

syscall(SYS_print_str, "--------test end----------\n");
```

![测试 3](picture/test3.png)

**结果分析：** 子进程成功输出 mmap 区、堆区和用户栈中的三个字符串，说明 `uvm_copy_pgtbl()` 复制了相应用户页，且 mmap 描述符链表也被正确复制。父进程在 `wait` 后得到 `num = 2`，并根据用户地址空间中收到的退出码输出 `good boy!`，验证了 `fork` 的父子返回值、`exit` 的 `ZOMBIE` 状态、`uvm_copyout()` 传递退出码以及 `proc_free()` 的回收路径。

### 测试 4：定时睡眠与唤醒

子进程请求睡眠 30 个 tick。调试输出显示它在每次 tick 后被唤醒检查、未到目标时再次睡眠；到达目标后输出 `Ready to exit!`，父进程随后被 `wait` 唤醒并输出 `Child exit!`。


```c
int pid = syscall(SYS_fork);

if(pid == 0){
    syscall(SYS_print_str, "Ready to sleep!\n");
    syscall(SYS_sleep, 30);
    syscall(SYS_print_str, "Ready to exit!\n");
    syscall(SYS_exit, 0);
}else{
    syscall(SYS_wait, 0);
    syscall(SYS_print_str, "Child exit!\n");
}

while(1)
    ;
```

![测试 4](picture/test4.png)

**结果分析：** PID 2 在等待期间反复进入睡眠，达到 30 个 tick 后才输出 `Ready to exit!`，说明 `timer_wait()` 会在每次被时钟唤醒后重新检查计时条件，而不会忙等。子进程退出后，等待子进程的 PID 1 被唤醒并输出 `Child exit!`，验证了 `proc_sleep()` 的锁交接、`proc_wakeup()` 的按通道唤醒以及 `proc_try_wakeup()` 的父进程唤醒逻辑。

## 总结

本次实验将 Lab 4 和 Lab 5 中的单一用户进程扩展为完整的多进程基本模型。实现过程中需要同时处理进程状态、锁、上下文、用户页表和物理资源的生命周期。

关键结论如下：

- `context` 用于进程与调度器之间的内核态切换，`trapframe` 用于用户态和内核态之间保存完整寄存器现场。
- 进程锁可跨越 `swtch()` 传递，用于保证状态变化和执行流切换的原子性。
- `ZOMBIE` 将“停止执行”和“释放资源”分离，父进程通过 `wait` 安全完成资源回收。
- `sleep/wakeup` 的关键是资源锁与进程锁之间的原子交接，既避免忙等，也避免丢失唤醒。
- 深拷贝页表、用户物理页和 mmap 描述符使 `fork` 后的父子进程拥有相互独立的用户地址空间。
