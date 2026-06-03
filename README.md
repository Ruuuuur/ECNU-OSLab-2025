# LAB-3: 中断和异常

本次实验在 LAB-1 的启动、UART 输出和自旋锁，以及 LAB-2 的物理内存和内核页表基础上，实现了内核态中断处理框架。完成后，内核可以响应 UART 外设中断和 CLINT 时钟中断，支持串口输入回显、换行、Backspace 删除，以及系统时钟 ticks 的维护。

## 实现内容

本次实验主要完成了以下内容：

- `start()`：进入 S-mode 前完成 trap 委托，并初始化 M-mode 时钟中断
- `timer_create()`：初始化系统时钟锁和 ticks
- `timer_update()`：在时钟中断中安全递增 ticks
- `timer_get_ticks()`：加锁读取当前 ticks
- `trap_kernel_handler()`：根据 `scause` 区分中断和异常，并分发 S-mode software interrupt 与 S-mode external interrupt
- `external_interrupt_handler()`：通过 PLIC 识别 UART 中断并调用 `uart_intr()`
- `uart_intr()`：支持普通字符回显、Enter 换行和 Backspace 删除
- `main()`：在合适位置接入 trap 初始化流程

## Trap 委托与初始化

内核主要运行在 S-mode，因此在 `start()` 从 M-mode 切换到 S-mode 之前，需要先配置 trap 委托。

异常通过 `medeleg` 委托给 S-mode：

```c
w_medeleg(0xffff);
```

大部分中断通过 `mideleg` 委托给 S-mode，但 M-mode timer interrupt 需要保留在 M-mode 处理：

```c
w_mideleg(0xffff & ~MIE_MTIE);
```

保留 M-mode timer interrupt 的原因是 CLINT 的 `mtime` 和 `mtimecmp` 相关寄存器只能由 M-mode 访问。时钟中断先进入 M-mode 的 `timer_vector`，更新下一次触发时间后，再设置 S-mode software interrupt pending bit，将处理流程转发给 S-mode。

`start()` 中还需要调用：

```c
timer_init();
```

该函数负责设置当前 CPU 的 `mtimecmp`、配置 `mscratch`、设置 `mtvec`，并打开 M-mode 时钟中断。

## S-mode Trap 入口

S-mode trap 入口由 `trap.S` 中的 `kernel_vector` 提供。每个 CPU 在 `trap_kernel_inithart()` 中执行：

```c
w_stvec((uint64)kernel_vector);
w_sie(r_sie() | SIE_SEIE | SIE_SSIE);
intr_on();
```

其中：

```text
SIE_SEIE: 允许 S-mode external interrupt，用于 UART/PLIC
SIE_SSIE: 允许 S-mode software interrupt，用于 M-mode timer 转发
```

`kernel_vector` 的主要职责是保存通用寄存器现场，调用 `trap_kernel_handler()`，恢复寄存器现场，最后通过 `sret` 返回原来的执行流。

## Trap 分发

`trap_kernel_handler()` 通过 `scause` 判断 trap 类型。

最高位用于区分中断和异常：

```c
if (scause & 0x8000000000000000ul) {
    // interrupt
} else {
    // exception
}
```

低位的 `trap_id` 用于进一步区分具体原因：

```c
int trap_id = scause & 0xf;
```

本实验需要处理两类 S-mode 中断：

```text
trap_id = 1: S-mode software interrupt，用于时钟中断转发
trap_id = 9: S-mode external interrupt，用于 UART 外设中断
```

对应分发逻辑为：

```c
case 1:
    timer_interrupt_handler();
    break;

case 9:
    external_interrupt_handler();
    break;
```

对于当前实验未处理的中断或异常，默认分支会打印 `sepc`、`stval` 和 `trap_id` 等调试信息，然后调用 `panic()` 终止。这符合本实验阶段对异常处理的要求。

## UART 外设中断

UART 中断通过 PLIC 转发给 S-mode。处理流程如下：

```text
UART 输入
-> PLIC
-> S-mode external interrupt
-> kernel_vector
-> trap_kernel_handler()
-> external_interrupt_handler()
-> uart_intr()
```

`external_interrupt_handler()` 中通过 `plic_claim()` 获取当前外设中断号。如果中断号是 `UART_IRQ`，则调用 `uart_intr()` 处理输入；处理完成后调用 `plic_complete(irq)` 告诉 PLIC 该中断已经完成。

UART 输入处理支持三种情况：

- 普通字符：原样回显
- Enter：将 `\r` 转换成 `\n` 输出
- Backspace / DEL：输出 `\b`、空格、`\b`，实现屏幕上的删除效果

Backspace 的三个输出含义是：

```text
\b: 光标左移一格
空格: 覆盖原字符
\b: 光标再次左移，停在被删除的位置
```

## 时钟中断

时钟中断分为 M-mode 和 S-mode 两段处理。

M-mode 部分由 `timer_init()` 和 `timer_vector` 协作完成：

- 设置当前 CPU 的 `mtimecmp`
- 设置 `mscratch`，供 `timer_vector` 暂存寄存器和读取 `mtimecmp` 地址
- 设置 `mtvec` 为 `timer_vector`
- 打开 M-mode timer interrupt
- 每次时钟中断时更新下一次 `mtimecmp`
- 设置 `sip.SSIP`，制造 S-mode software interrupt

S-mode 部分由 `timer_interrupt_handler()` 完成：

```c
if(mycpuid() == 0){
    timer_update();
}

w_sip(r_sip() & ~2);
```

因为多个 CPU 都可能收到时钟中断，而 `ticks` 是共享资源，所以只让 CPU0 更新系统 ticks。`timer_update()` 内部使用自旋锁保护 `ticks++`，保证并发安全。

最后通过清除 `sip.SSIP` 宣布 S-mode software interrupt 处理完成。

## 初始化顺序

CPU0 负责全局初始化：

```c
print_init();
pmem_init();
kvm_init();
kvm_inithart();
trap_kernel_init();
trap_kernel_inithart();
```

其他 CPU 等待 CPU0 完成全局初始化后，只需要完成本 CPU 独有的页表和 trap 初始化：

```c
kvm_inithart();
trap_kernel_inithart();
```

`trap_kernel_init()` 只由 CPU0 调用一次，因为它初始化的是共享的 PLIC 优先级和系统时钟。`trap_kernel_inithart()` 每个 CPU 都要调用，因为 `stvec`、`sie` 和 PLIC hart 配置都属于每个 CPU 自己的状态。

## 测试结果

### 编译测试

测试目标：确认 lab3 实现后可以从干净状态完整构建。

测试命令：

```bash
make clean && make build
```

测试结果：构建通过，仅出现链接器关于 RWX segment 的 warning，该 warning 在当前实验框架中可以忽略。

```text
riscv64-linux-gnu-ld: warning: target/kernel/kernel-qemu.elf has a LOAD segment with RWX permissions
```

### 时钟滴答测试

测试目标：确认 CLINT timer interrupt 能周期性触发，并最终进入 S-mode 的 `timer_interrupt_handler()` 更新 ticks。

测试方法：临时在 `timer_interrupt_handler()` 中加入 ticks 输出，只在 CPU0 更新并打印 ticks。

临时代码：

```c
if(mycpuid() == 0){
    timer_update();
    printf("cpu: %d, ticks = %d\n", mycpuid(), (int)timer_get_ticks());
}
```

运行后可以观察到 ticks 持续递增，说明下面的链路已经打通：

```text
CLINT timer interrupt
-> M-mode timer_vector
-> 设置 sip.SSIP
-> S-mode software interrupt
-> kernel_vector
-> trap_kernel_handler()
-> timer_interrupt_handler()
```

测试完成后删除临时 `printf`，正式代码中只保留 `timer_update()`。

运行结果：

![时钟滴答测试](pictures/dida%20test.png)

### UART 输入测试

测试目标：确认 UART 外设中断可以被 PLIC 转发并由内核处理，同时验证普通字符、Enter 和 Backspace 的回显行为。

测试方式：运行内核后，在 QEMU 终端输入字符并观察输出。

测试要点：

- 输入普通字符，可以正常回显
- 按 Enter，可以正确换行
- 按 Backspace，可以删除屏幕上的前一个字符

测试链路：

```text
UART input
-> PLIC
-> S-mode external interrupt
-> kernel_vector
-> trap_kernel_handler()
-> external_interrupt_handler()
-> uart_intr()
```

运行结果：

![UART 输入测试](pictures/UART%20input%20test.png)

## 实验结论

本次实验完成了内核态中断处理的基础框架。内核能够将大部分 trap 委托给 S-mode，同时保留 M-mode timer interrupt 以访问 CLINT 时钟寄存器。通过 `timer_vector` 将 M-mode timer interrupt 转换为 S-mode software interrupt 后，内核可以在 S-mode 中统一维护系统 ticks。

UART 部分通过 PLIC 接收外设中断，并在 `external_interrupt_handler()` 中识别 `UART_IRQ` 后调用 `uart_intr()`。最终实现了串口输入回显、换行和 Backspace 删除。

实验完成后，内核可以正常启动两个 CPU，响应时钟中断和 UART 输入中断，为后续进程、系统调用和调度相关实验提供了基础中断机制。
