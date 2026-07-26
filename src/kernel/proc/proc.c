#include "mod.h"
#include "../../user/initcode.h"

#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t *old, context_t *new);

// in trap/trap_user.c
extern void trap_user_return();

/* ------------本地变量----------- */

// 进程结构体数组 + 第一个用户进程的指针
static proc_t proc_list[N_PROC];
static proc_t *proczero;

// 全局pid + 保护它的锁
static int global_pid;
static spinlock_t pid_lk;

/* 获取一个pid */
static int alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&pid_lk);
    assert(global_pid > 0, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&pid_lk);
    return tmp;
}

/* 释放进程锁 + trap_user_return */
static void proc_return()
{
    proc_t *p = myproc();

    assert(p != NULL,
        "proc_return: no current process");
    assert(spinlock_holding(&p->lk),
        "proc_return: process lock is not held");

    spinlock_release(&p->lk);
    trap_user_return();

    panic("proc_return: trap_user_return returned");
}

/* 进程模块初始化 */
void proc_init()
{
    spinlock_init(&pid_lk, "pid");
    global_pid = 1;

    for(int i = 0; i < N_PROC; ++i){
        proc_t *p = &proc_list[i];

        memset(p, 0, sizeof(*p));
        spinlock_init(&p->lk, "proc");

        p->state = UNUSED;
        p->kstack = KSTACK(i);
    }
}

/*
    申请一个UNUSED进程结构体(返回时带锁)
    并执行通用的初始化逻辑
*/
proc_t *proc_alloc()
{
    proc_t *p;

    for(p = proc_list; p < proc_list + N_PROC; ++p){
        spinlock_acquire(&p->lk);

        if(p->state == UNUSED){
            break;
        }

        spinlock_release(&p->lk);
    }

    if(p == proc_list + N_PROC){
        return NULL;
    }

    //此时仍持有p->lk
    p->pid = alloc_pid();
    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;

    memset(p->name, 0, sizeof(p->name));
    memset(&p->ctx, 0, sizeof(p->ctx));

    p->heap_top = USER_BASE + PGSIZE;
    p->ustack_npage = 1;
    p->mmap = NULL;

    p->tf = (trapframe_t *)pmem_alloc(true);
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);

    p->ctx.ra = (uint64)proc_return;
    p->ctx.sp = p->kstack + PGSIZE;

    return p;
}

/*
    回收一个进程结构体并释放它包含的资源
    tips: 调用者需要持有进程锁
*/
void proc_free(proc_t *p)
{
    assert(p != NULL, "proc_free: p is NULL");
    assert(spinlock_holding(&p->lk),
        "proc_free: process lock is not held");
    assert(p->state != UNUSED,
        "proc_free: process already unused");

    /*
     * 释放用户页、trapframe 和三级页表。
     * trampoline 只解除映射，不释放共享物理页。
     */

    if(p->pgtbl != NULL){
        uvm_destroy_pgtbl(p->pgtbl);
    }

    /*
     * uvm_destroy_pgtbl() 只销毁页表和物理页，
     * mmap 描述符还要归还 mmap 仓库。
     */
    mmap_region_t *mmap = p->mmap;
    while(mmap != NULL){
        mmap_region_t *next = mmap->next;
        mmap_region_free(mmap);
        mmap = next;
    }

    p->pid = 0;
    memset(p->name, 0, sizeof(p->name));

    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;

    p->pgtbl = NULL;
    p->heap_top = 0;
    p->ustack_npage = 0;
    p->mmap = NULL;
    p->tf = NULL;

    memset(&p->ctx, 0, sizeof(p->ctx));

    // 最后再公开为空闲槽位
    p->state = UNUSED;
}

/*
    获得一个初始化过的用户页表
    完成trapframe和trampoline的映射
*/
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    memset(pgtbl, 0, PGSIZE);

    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline,
        PGSIZE, PTE_R | PTE_X);
    vm_mappages(pgtbl, TRAPFRAME, trapframe,
        PGSIZE, PTE_R | PTE_W);

    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问
*/
void proc_make_first()
{
    proczero = proc_alloc();
    assert(proczero != NULL,
        "proc_make_first: no free process");

    proc_t *p = proczero;

    // proc_alloc() 返回时仍持有 p->lk
    memmove(p->name, "proczero", sizeof("proczero"));

    assert(initcode_len <= PGSIZE,
        "proc_make_first: initcode too large");

    // 用户代码页
    void *ucode = pmem_alloc(false);
    memmove(ucode, initcode, initcode_len);

    vm_mappages(
        p->pgtbl,
        USER_BASE,
        (uint64)ucode,
        PGSIZE,
        PTE_R | PTE_W | PTE_X | PTE_U
    );

    // 初始用户栈页
    void *ustack = pmem_alloc(false);

    vm_mappages(
        p->pgtbl,
        TRAPFRAME - PGSIZE,
        (uint64)ustack,
        PGSIZE,
        PTE_R | PTE_W | PTE_U
    );

    // 第一次进入用户态时的 PC 和 SP
    p->tf->user_to_kern_epc = USER_BASE;
    p->tf->sp = TRAPFRAME;

    // 初始化完成，交给调度器
    p->state = RUNNABLE;
    spinlock_release(&p->lk);
}

/*
    父进程产生子进程
    UNUSED -> RUNNABLE
*/
int proc_fork()
{
    proc_t *parent = myproc();
    proc_t *child = proc_alloc();

    if(child == NULL){
        return -1;
    }

    // proc_alloc() 返回时持有 child->lk

    // 复制用户态寄存器现场
    memmove(child->tf, parent->tf, sizeof(*parent->tf));

    // 复制用户地址空间中的实际物理页
    uvm_copy_pgtbl(
        parent->pgtbl,
        child->pgtbl,
        parent->heap_top,
        parent->ustack_npage,
        parent->mmap
    );

    child->heap_top = parent->heap_top;
    child->ustack_npage = parent->ustack_npage;
    child->parent = parent;

    memmove(child->name, parent->name, sizeof(child->name));

    // 深拷贝 mmap 描述符链表
    mmap_region_t *last = NULL;

    for(mmap_region_t *src = parent->mmap;
        src != NULL;
        src = src->next){

        mmap_region_t *dst = mmap_region_alloc();
        dst->begin = src->begin;
        dst->npages = src->npages;
        dst->next = NULL;

        if(last == NULL){
            child->mmap = dst;
        }else{
            last->next = dst;
        }

        last = dst;
    }

    // fork 在子进程中返回 0
    child->tf->a0 = 0;

    int pid = child->pid;

    child->state = RUNNABLE;
    spinlock_release(&child->lk);

    // fork 在父进程中返回子进程 PID
    return pid;
}

/*
    进程主动放弃CPU控制权
    RUNNING->RUNNABLE
*/
void proc_yield()
{
    proc_t *p = myproc();

    assert(p != NULL, "proc_yield: no current process");

    spinlock_acquire(&p->lk);
    assert(p->state == RUNNING,
        "proc_yield: process is not running");

    p->state = RUNNABLE;
    proc_sched();

    spinlock_release(&p->lk);
}

/*
    当父进程退出时, 让它的所有子进程认proczero为父
    因为proczero永不退出, 可以回收子进程的资源
*/
static void proc_reparent(proc_t *parent)
{
    assert(parent != NULL,
        "proc_reparent: parent is NULL");

    for(proc_t *p = proc_list;p < proc_list + N_PROC;++p){

        // 避免重复获取当前退出进程自己的锁
        if(p == parent){
            continue;
        }

        spinlock_acquire(&p->lk);

        if(p->state != UNUSED && p->parent == parent){
            p->parent = proczero;
        }

        spinlock_release(&p->lk);
    }
}

/*
    唤醒等待呼叫的进程
    由proc_exit调用
    tips: 调用者需要持有p的进程锁
*/
static void proc_try_wakeup(proc_t *p)
{
    assert(p != NULL,
        "proc_try_wakeup: p is NULL");
    assert(spinlock_holding(&p->lk),
        "proc_try_wakeup: process lock is not held");

    if(p->state == SLEEPING &&
        p->sleep_space == p){

        p->state = RUNNABLE;
    }
}

/*
    进程退出
    RUNNING -> ZOMBIE
*/
void proc_exit(int exit_code)
{
    proc_t *p = myproc();

    assert(p != NULL,
        "proc_exit: no current process");
    assert(p != proczero,
        "proc_exit: proczero cannot exit");

    // 当前进程的孩子全部交给 proczero
    proc_reparent(p);

    proc_t *parent = p->parent;
    assert(parent != NULL,
        "proc_exit: no parent");

    /*
     * 锁顺序必须是父进程锁 -> 子进程锁，
     * 与后面的 proc_wait() 保持一致。
     */
    spinlock_acquire(&parent->lk);
    spinlock_acquire(&p->lk);

    assert(p->state == RUNNING,
        "proc_exit: process is not running");

    p->exit_code = exit_code;
    p->state = ZOMBIE;

    // 此时持有 parent->lk
    proc_try_wakeup(parent);

    /*
     * proc_sched() 要求只持有当前进程锁，
     * 所以先释放父进程锁。
     */
    spinlock_release(&parent->lk);

    proc_sched();

    panic("proc_exit: zombie returned");
}

/*
    父进程等待一个子进程进入ZOMBIE状态
    1. 如果等到: 释放子进程, 返回子进程的pid, 将子进程的退出状态传出到user_addr
    2. 如果发现没孩子: 返回-1
    3. 如果没等到: 父进程进入睡眠状态
*/
int proc_wait(uint64 user_addr)
{
    proc_t *parent = myproc();

    assert(parent != NULL,
        "proc_wait: no current process");

    spinlock_acquire(&parent->lk);

    while(1){
        bool have_child = false;

        for(proc_t *child = proc_list;
            child < proc_list + N_PROC;
            ++child){

            if(child == parent){
                continue;
            }

            spinlock_acquire(&child->lk);

            if(child->parent == parent){
                have_child = true;

                if(child->state == ZOMBIE){
                    int pid = child->pid;

                    if(user_addr != 0){
                        uvm_copyout(
                            parent->pgtbl,
                            user_addr,
                            (uint64)&child->exit_code,
                            sizeof(child->exit_code)
                        );
                    }

                    proc_free(child);

                    spinlock_release(&child->lk);
                    spinlock_release(&parent->lk);

                    return pid;
                }
            }

            spinlock_release(&child->lk);
        }

        if(!have_child){
            spinlock_release(&parent->lk);
            return -1;
        }

        /*
         * 有孩子，但没有孩子进入 ZOMBIE。
         * 父进程以自己为等待资源进入睡眠。
         *
         * proc_sleep() 返回时会重新持有 parent->lk。
         */
        proc_sleep(parent, &parent->lk);
    }
}

/*
    进程等待sleep_space对应的资源, 进入睡眠状态
    RUNNING -> SLEEPING
*/
void proc_sleep(void *sleep_space, spinlock_t *lock)
{
    proc_t *p = myproc();

    assert(p != NULL,
        "proc_sleep: no current process");
    assert(sleep_space != NULL,
        "proc_sleep: sleep space is NULL");
    assert(lock != NULL,
        "proc_sleep: lock is NULL");
    assert(spinlock_holding(lock),
        "proc_sleep: lock is not held");

    /*
     * 如果传入的不是当前进程锁：
     * 先取得进程锁，再释放资源锁。
     */
    if(lock != &p->lk){
        spinlock_acquire(&p->lk);
        spinlock_release(lock);
    }

    assert(p->state == RUNNING,
        "proc_sleep: process is not running");

    p->sleep_space = sleep_space;
    p->state = SLEEPING;

    proc_sched();

    /*
     * 被唤醒并重新调度后从这里继续，
     * 此时调度器再次把 p->lk 交给了当前进程。
     */
    p->sleep_space = NULL;

    /*
     * 恢复调用前的锁状态：
     * 释放进程锁，重新取得原来的资源锁。
     */
    if(lock != &p->lk){
        spinlock_release(&p->lk);
        spinlock_acquire(lock);
    }
}

/*
    唤醒所有等待sleep_space的进程
    SLEEPING -> RUNNABLE
*/
void proc_wakeup(void *sleep_space)
{
    assert(sleep_space != NULL,
        "proc_wakeup: sleep space is NULL");

    proc_t *current = myproc();

    for(proc_t *p = proc_list;
        p < proc_list + N_PROC;
        ++p){

        /*
         * 当前正在执行 proc_wakeup() 的进程不可能处于睡眠态，
         * 而且它可能已经持有自己的进程锁。
         */
        if(p == current){
            continue;
        }

        spinlock_acquire(&p->lk);

        if(p->state == SLEEPING &&
            p->sleep_space == sleep_space){

            p->state = RUNNABLE;
        }

        spinlock_release(&p->lk);
    }
}

/*
    用户进程切换到调度器
    tips: 调用者保证持有当前进程的锁
*/
void proc_sched()
{
    proc_t *p = myproc();
    cpu_t *cpu = mycpu();

    assert(p != NULL,
        "proc_sched: no current process");
    assert(spinlock_holding(&p->lk),
        "proc_sched: process lock is not held");
    assert(cpu->noff == 1,
        "proc_sched: unexpected lock count");
    assert(p->state != RUNNING,
        "proc_sched: process is still running");
    assert(intr_get() == 0,
        "proc_sched: interrupt enabled");

    int origin = cpu->origin;

    swtch(&p->ctx, &cpu->ctx);

    cpu->origin = origin;
}

/*
    调度器
    RUNNABLE->RUNNING
*/
void proc_scheduler()
{
    cpu_t *cpu = mycpu();
    cpu->proc = NULL;

    while(1){
        intr_on();

        for(proc_t *p = proc_list; p < proc_list + N_PROC; ++p){
            spinlock_acquire(&p->lk);

            if(p->state == RUNNABLE){
                p->state = RUNNING;
                cpu->proc = p;

                swtch(&cpu->ctx, &p->ctx);

                cpu->proc = NULL;
            }

            spinlock_release(&p->lk);
        }
    }
}
