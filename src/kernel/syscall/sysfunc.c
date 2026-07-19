#include "mod.h"

/*
    用户堆空间伸缩
    uint64 new_heap_top (如果是0, 代表查询当前堆顶位置)
    成功返回new_heap_top, 失败返回-1
*/
uint64 sys_brk()
{
    proc_t *p = myproc();

    uint64 new_heap_top;
    arg_uint64(0, &new_heap_top);

    uint64 old_heap_top = p->heap_top;
    uint64 heap_begin = USER_BASE + PGSIZE;

    if (new_heap_top == 0)
        return old_heap_top;

    if(new_heap_top < heap_begin || new_heap_top > MMAP_BEGIN)
        return -1;

    if(new_heap_top == old_heap_top)
        return old_heap_top;

    uint64 len;
    uint64 result;

    if(new_heap_top > old_heap_top){
        len = new_heap_top - old_heap_top;

        if(len > 0xfffffffful)
            return -1;

        result = uvm_heap_grow(
            p->pgtbl, old_heap_top, (uint32)len);
    }else{
        len = old_heap_top - new_heap_top;

        if(len > 0xfffffffful)
            return -1;

        result = uvm_heap_ungrow(
            p->pgtbl, old_heap_top, (uint32)len);
    }

    if(result == (uint64)-1)
        return -1;

    p->heap_top = result;
    return result;
}

/*
    增加一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节,需检查是否是page-aligned)
    成功返回映射空间的起始地址, 失败返回-1
*/
uint64 sys_mmap()
{
    uint64 begin;
    uint32 len;

    arg_uint64(0, &begin);
    arg_uint32(1, &len);

    if(len == 0 || len % PGSIZE != 0)
        return -1;

    if(begin != 0){
        if(begin % PGSIZE != 0)
            return -1;

        if(begin < MMAP_BEGIN || begin >= MMAP_END)
            return -1;

        if((uint64)len > MMAP_END - begin)
            return -1;
    }

    return uvm_mmap(
        begin,
        len / PGSIZE,
        PTE_R | PTE_W
    );
}

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节, 需检查是否是page-aligned)
    成功返回0 失败返回-1
*/
uint64 sys_munmap()
{
    uint64 begin;
    uint32 len;

    arg_uint64(0, &begin);
    arg_uint32(1, &len);

    if(len == 0 || len % PGSIZE != 0)
        return -1;

    if(begin % PGSIZE != 0)
        return -1;

    if(begin < MMAP_BEGIN || begin >= MMAP_END)
        return -1;

    if((uint64)len > MMAP_END - begin)
        return -1;

    uvm_munmap(begin, len / PGSIZE);

    return 0;
}

/*
    打印一个字符串
    char *str
    成功返回0
*/
uint64 sys_print_str()
{

}

/*
    打印一个32位整数
    int num
    成功返回0
*/
uint64 sys_print_int()
{

}

/*
    进程复制
    返回子进程的pid
*/
uint64 sys_fork()
{

}

/*
    等待子进程退出
    uint64 addr_exit_state
*/
uint64 sys_wait()
{

}

/*
    进程退出
    int exit_code
    不返回
*/
uint64 sys_exit()
{

}

/*
    让进程睡眠一段时间
    uint32 ntick (1个tick大约0.1秒)
    成功返回0
*/
uint64 sys_sleep()
{

}

/*
    返回当前进程的pid
*/
uint64 sys_getpid()
{

}
