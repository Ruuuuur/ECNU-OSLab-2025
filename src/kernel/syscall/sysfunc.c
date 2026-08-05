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
                p->pgtbl, old_heap_top, (uint32)len,
                PTE_R | PTE_W);
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
    char path[STR_MAXLEN + 1];
    uint64 user_argv;

    arg_str(0, path, sizeof(path));
    arg_uint64(1, &user_argv);

    if (user_argv == 0)
        return -1;

    proc_t *p = myproc();

    /*
     * 所有参数字符串最多占用：
     * ELF_MAXARGS * ELF_MAXARG_LEN = 4096字节。
     * 使用独立物理页，避免撑爆4KB内核栈。
     */
    char *arg_page =
        (char *)pmem_alloc(true);

    char *kernel_argv[ELF_MAXARGS + 1];
    int result = -1;

    for (uint32 argc = 0;
        argc <= ELF_MAXARGS;
        argc++) {
        uint64 user_arg;

        /*
         * 先从用户态argv数组中取出一个字符串地址。
         */
        uvm_copyin(
            p->pgtbl,
            (uint64)&user_arg,
            user_argv +
                (uint64)argc * sizeof(uint64),
            sizeof(user_arg)
        );

        /*
         * NULL表示argv数组结束。
         */
        if (user_arg == 0) {
            kernel_argv[argc] = NULL;
            result = proc_exec(path, kernel_argv);
            break;
        }

        /*
         * 第33个指针仍不为NULL，说明参数过多。
         */
        if (argc == ELF_MAXARGS)
            break;

        kernel_argv[argc] =
            arg_page + argc * ELF_MAXARG_LEN;

        uvm_copyin_str(
            p->pgtbl,
            (uint64)kernel_argv[argc],
            user_arg,
            ELF_MAXARG_LEN
        );
    }

    pmem_free((uint64)arg_page, true);
    return result;
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
    char path[STR_MAXLEN + 1];
    uint32 open_mode;

    arg_str(0, path, sizeof(path));
    arg_uint32(1, &open_mode);

    file_t *file =
        file_open(path, open_mode);

    if (file == NULL)
        return -1;

    uint32 fd = alloc_fd(file);

    /*
     * 进程文件描述符表已满，需要撤销file_open。
     */
    if (fd == (uint32)-1) {
        file_close(file);
        return -1;
    }

    return fd;
}

/*
    关闭文件
    uint32 fd
    成功返回0, 失败返回-1
*/
uint64 sys_close()
{
    uint32 fd;
    file_t *file;

    if (arg_fd(0, &fd, &file) < 0)
        return -1;

    /*
     * 先解除当前进程的fd映射，再归还file引用。
     */
    myproc()->open_file[fd] = NULL;
    file_close(file);

    return 0;
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
    file_t *file;
    uint32 len;
    uint64 addr;

    if (arg_fd(0, NULL, &file) < 0)
        return 0;

    arg_uint32(1, &len);
    arg_uint64(2, &addr);

    if (len > 0 && addr == 0)
        return 0;

    return file_read(
        file,
        len,
        addr,
        true
    );
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
    file_t *file;
    uint32 len;
    uint64 addr;

    if (arg_fd(0, NULL, &file) < 0)
        return 0;

    arg_uint32(1, &len);
    arg_uint64(2, &addr);

    if (len > 0 && addr == 0)
        return 0;

    return file_write(
        file,
        len,
        addr,
        true
    );
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
    file_t *file;
    uint32 offset;
    uint32 flag;

    if (arg_fd(0, NULL, &file) < 0)
        return (uint32)-1;

    arg_uint32(1, &offset);
    arg_uint32(2, &flag);

    return file_lseek(
        file,
        offset,
        flag
    );
}

/*
    复制文件控制权
    uinr32 fd
    成功返回new_fd, 失败返回-1
*/
uint64 sys_dup()
{
    file_t *file;

    if (arg_fd(0, NULL, &file) < 0)
        return (uint32)-1;

    /*
     * 新fd和原fd指向同一个file_t。
     */
    file_t *new_file = file_dup(file);

    uint32 new_fd = alloc_fd(new_file);

    /*
     * 当前进程没有空闲fd，撤销刚增加的引用。
     */
    if (new_fd == (uint32)-1) {
        file_close(new_file);
        return (uint32)-1;
    }

    return new_fd;
}

/*
    获取文件信息
    uint32 fd
    uint64 addr
    成功返回0, 失败返回-1
*/
uint64 sys_fstat()
{
    file_t *file;
    uint64 addr;

    if (arg_fd(0, NULL, &file) < 0)
        return (uint32)-1;

    arg_uint64(1, &addr);

    if (addr == 0)
        return (uint32)-1;

    return file_get_stat(file, addr);
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
    file_t *file;
    uint64 addr;
    uint32 len;

    if (arg_fd(0, NULL, &file) < 0)
        return (uint32)-1;

    arg_uint64(1, &addr);
    arg_uint32(2, &len);

    if (file->ip == NULL ||
        !file->readable ||
        (len > 0 && addr == 0))
        return (uint32)-1;

    inode_t *ip = file->ip;

    inode_lock(ip);
    bool is_dir =
        ip->disk_info.type == INODE_TYPE_DIR;
    inode_unlock(ip);

    if (!is_dir)
        return (uint32)-1;

    return file_read(
        file,
        len,
        addr,
        true
    );
}

/*
    创建目录
    char *path
    成功返回0, 失败返回-1
 */
uint64 sys_mkdir()
{
    char path[STR_MAXLEN + 1];

    arg_str(0, path, sizeof(path));

    inode_t *ip = path_create_inode(
        path,
        INODE_TYPE_DIR,
        INODE_MAJOR_DEFAULT,
        INODE_MINOR_DEFAULT
    );

    if (ip == NULL)
        return (uint32)-1;

    /*
     * path_create_inode() 返回时带有一次inode引用。
     * 系统调用只负责创建，不继续持有该inode。
     */
    inode_put(ip);

    return 0;
}

/*
    修改当前工作目录
    char *new_path
    成功返回0, 失败返回-1
 */
uint64 sys_chdir()
{
    char path[STR_MAXLEN + 1];

    arg_str(0, path, sizeof(path));

    inode_t *new_cwd =
        path_to_inode(path);

    if (new_cwd == NULL)
        return (uint32)-1;

    inode_lock(new_cwd);

    bool is_dir =
        new_cwd->disk_info.type == INODE_TYPE_DIR;

    inode_unlock(new_cwd);

    if (!is_dir) {
        inode_put(new_cwd);
        return (uint32)-1;
    }

    proc_t *p = myproc();
    inode_t *old_cwd = p->cwd;

    /*
     * 先接管新cwd，再归还旧cwd引用。
     */
    p->cwd = new_cwd;

    if (old_cwd != NULL)
        inode_put(old_cwd);

    return 0;
}

/*
    打印当前工作目录的绝对路径
    成功返回0, 失败返回-1
 */
uint64 sys_print_cwd()
{
    proc_t *p = myproc();

    if (p == NULL || p->cwd == NULL)
        return (uint32)-1;

    char path[STR_MAXLEN + 1];

    uint32 offset = inode_to_path(
        p->cwd,
        path,
        sizeof(path)
    );

    if (offset == (uint32)-1)
        return (uint32)-1;

    printf(
        "current work directory = %s\n",
        path + offset
    );

    return 0;
}

/*
    新建链接
    char *old_path
    char *new_path
    成功返回0, 失败返回-1
 */
uint64 sys_link()
{
    char old_path[STR_MAXLEN + 1];
    char new_path[STR_MAXLEN + 1];

    arg_str(0, old_path, sizeof(old_path));
    arg_str(1, new_path, sizeof(new_path));

    return path_link(
        old_path,
        new_path
    );
}

/*
    删除链接 (可能触发删除文件)
    char *path
    成功返回0, 失败返回-1
 */
uint64 sys_unlink()
{
    char path[STR_MAXLEN + 1];

    arg_str(0, path, sizeof(path));

    return path_unlink(path);
}
