#include "mod.h"
#include "../../user/syscall_num.h"
// in trampoline.S
extern char trampoline[];  // 内核和用户切换的代码
extern char user_vector[]; // 用户触发陷阱进入内核
extern char user_return[]; // 内核处理完毕返回用户

// in trap.S
extern char kernel_vector[]; // 内核态trap处理流程, 进入内核后应当切换中断处理入口

// in trap_kernel.c
extern char *interrupt_info[16]; // 中断错误信息
extern char *exception_info[16]; // 异常错误信息

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler()
{
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();
    uint64 stval = r_stval();

    assert((sstatus & SSTATUS_SPP) == 0, "trap_user_handler: not from u-mode");
    assert(intr_get() == 0, "trap_user_handler: interrupt enabled");

    w_stvec((uint64)kernel_vector);

    proc_t *p = myproc();
    p->tf->user_to_kern_epc = sepc;

    int trap_id = scause & 0xf;

    if(scause & 0x8000000000000000ul) { //中断
        switch(trap_id){

        case 1:
            timer_interrupt_handler(); 
            break;

        case 9:
            external_interrupt_handler();
            break;
        default:
            printf("\nunexpected interrupt: %s\n", interrupt_info[trap_id]);
            printf("trap_id = %d, sepc = %p, stval = %p\n", trap_id, sepc, stval);
            panic("trap_user_handler");
        }
    }else{ //异常
        switch(trap_id){

        case 8:
            p->tf->user_to_kern_epc += 4;
            syscall();
            break;
        case 13:
        case 15:
        {
            uint64 old_npage = p->ustack_npage;
            uint64 new_npage = uvm_ustack_grow(
                p->pgtbl,
                old_npage,
                stval
            );

            if(new_npage == (uint64)-1){
                printf("invalid stack page fault: %p\n", stval);
                panic("trap_user_handler: stack grow failed");
            }

            p->ustack_npage = new_npage;
            break;
        }

        default:
            printf("\nunexpected exception: %s\n", exception_info[trap_id]);
            printf("trap_id = %d, sepc = %p, stval = %p\n", trap_id, sepc, stval);
            panic("trap_user_handler");
        }
    }

    trap_user_return();
}

// 调用user_return()
// 内核态返回用户态
void trap_user_return()
{   
    proc_t *p = myproc();

    intr_off();

    uint64 user_vector_addr = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);
    w_stvec(user_vector_addr);

    p->tf->user_to_kern_satp = r_satp();
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    p->tf->user_to_kern_hartid = r_tp();

    w_sepc(p->tf->user_to_kern_epc);

    uint64 sstatus = r_sstatus();
    sstatus &= ~SSTATUS_SPP;
    sstatus |= SSTATUS_SPIE;
    w_sstatus(sstatus);

    uint64 user_return_addr = TRAMPOLINE + ((uint64)user_return - (uint64)trampoline);
    void (*fn)(uint64, uint64) = (void (*)(uint64, uint64))user_return_addr;

    fn(TRAPFRAME, MAKE_SATP(p->pgtbl));

}
