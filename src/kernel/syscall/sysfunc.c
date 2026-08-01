#include "mod.h"
#include "../proc/method.h"
#include "../trap/method.h"
#include "../fs/mod.h"
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
    进程复制
    返回子进程的pid
*/
uint64 sys_fork()
{
    return proc_fork();
}

/*
    等待子进程退出
    uint64 addr_exit_state
*/
uint64 sys_wait()
{
    uint64 user_addr;

    arg_uint64(0, &user_addr);

    return proc_wait(user_addr);
}

/*
    进程退出
    int exit_code
    不返回
*/
uint64 sys_exit()
{
    uint32 exit_code;

    arg_uint32(0, &exit_code);

    proc_exit((int)exit_code);

    panic("sys_exit: proc_exit returned");
    return -1;
}

/*
    让进程睡眠一段时间
    uint32 ntick (1个tick大约0.1秒)
    成功返回0
*/
uint64 sys_sleep()
{
    uint32 ntick;

    arg_uint32(0, &ntick);
    timer_wait(ntick);

    return 0;
}

/*
    返回当前进程的pid
*/
uint64 sys_getpid()
{
    proc_t *p = myproc();

    assert(p != NULL,
        "sys_getpid: no current process");

    return p->pid;
}

/*
    执行ELF文件以替换当前进程的内容
    char *path
    char **argv
    成功返回argc, 失败返回-1
*/
uint64 sys_exec()
{

}

/* 构建fd->file的映射, 返回fd */
static uint32 alloc_fd(file_t *file)
{
    proc_t *p = myproc();
    for (uint32 i = 0; i < N_OPEN_FILE_PER_PROC; i++)
    {
        if (p->open_file[i] == NULL) {
            p->open_file[i] = file;
            return i;
        }
    }
    return -1;
}

/*
    打开或创建文件
    char *path
    uint32 open_mode
    成功返回fd, 失败返回-1
*/
uint64 sys_open()
{

}

/*
    关闭文件
    uint32 fd
    成功返回0, 失败返回-1
*/
uint64 sys_close()
{

}

/*
    读取文件内容
    uint32 fd
    uint32 len
    uint64 addr
    成功返回读到的字节数, 失败返回0
*/
uint64 sys_read()
{

}

/*
    写入文件内容
    uint32 fd
    uint32 len
    uint64 addr
    成功返回写入的字节数, 失败返回0
*/
uint64 sys_write()
{

}

/*
    调整读写指针位置
    uint32 fd
    uint32 offset
    uint32 flag
    成功返回新的偏移量, 失败返回-1
*/
uint64 sys_lseek()
{

}

/*
    复制文件控制权
    uinr32 fd
    成功返回new_fd, 失败返回-1
*/
uint64 sys_dup()
{

}

/*
    获取文件信息
    uint32 fd
    uint64 addr
    成功返回0, 失败返回-1
*/
uint64 sys_fstat()
{

}

/*
    获取目录中的所有目录项信息
    uint32 fd
    uint64 addr
    uint32 buffer_len
    成功返回读到的字节数, 失败返回-1
*/
uint64 sys_get_dentries()
{

}

/*
    创建目录
    char *path
    成功返回0, 失败返回-1
 */
uint64 sys_mkdir()
{

}

/*
    修改当前工作目录
    char *new_path
    成功返回0, 失败返回-1
 */
uint64 sys_chdir()
{

}

/*
    打印当前工作目录的绝对路径
    成功返回0, 失败返回-1
 */
uint64 sys_print_cwd()
{

}

/*
    新建链接
    char *old_path
    char *new_path
    成功返回0, 失败返回-1
 */
uint64 sys_link()
{

}

/*
    删除链接 (可能触发删除文件)
    char *path
    成功返回0, 失败返回-1
 */
uint64 sys_unlink()
{

}
