#include "mod.h"

super_block_t sb; /* 超级块 */

file_t file_table[N_FILE]; // 文件资源池
spinlock_t lk_file_table; // 保护它的锁

/* 初始化file_table */
void file_init()
{
	spinlock_init(
        &lk_file_table,
        "file_table"
    );

    memset(
        file_table,
        0,
        sizeof(file_table)
    );
}

/* 从file_table中获取1个空闲file */
file_t* file_alloc()
{
	spinlock_acquire(&lk_file_table);

    for (uint32 i = 0; i < N_FILE; i++) {
        file_t *file = &file_table[i];

        if (file->ref != 0)
            continue;

        /*
         * 必须在释放锁之前将ref设为1，
         * 这样其他CPU不会再次取得这个槽位。
         */
        file->ref = 1;
        file->ip = NULL;
        file->readable = false;
        file->writbale = false;
        file->offset = 0;

        spinlock_release(&lk_file_table);
        return file;
    }

    spinlock_release(&lk_file_table);
    return NULL;
}

/*
	根据路径打开文件 (指定打开模式)
	成功返回file, 失败返回NULL
*/
file_t* file_open(char *path, uint32 open_mode)
{
	uint32 valid_mode =
        FILE_OPEN_CREATE |
        FILE_OPEN_READ |
        FILE_OPEN_WRITE;

    if (path == NULL ||
        path[0] == '\0' ||
        (open_mode & ~valid_mode) != 0 ||
        (open_mode &
            (FILE_OPEN_READ | FILE_OPEN_WRITE)) == 0) {
        return NULL;
    }

    /*
     * 先尝试打开已有文件。
     */
    inode_t *ip = path_to_inode(path);

    /*
     * 文件不存在且指定CREATE时，创建普通数据文件。
     */
    if (ip == NULL &&
        (open_mode & FILE_OPEN_CREATE)) {
        ip = path_create_inode(
            path,
            INODE_TYPE_DATA,
            INODE_MAJOR_DEFAULT,
            INODE_MINOR_DEFAULT
        );
    }

    if (ip == NULL)
        return NULL;

    /*
     * 检查inode类型和设备号。
     */
    inode_lock(ip);
    uint16 type = ip->disk_info.type;
    uint16 major = ip->disk_info.major;
    inode_unlock(ip);

    /*
     * 目录不能以写模式打开。
     * 设备文件由设备层检查读写权限。
     */
    if ((type == INODE_TYPE_DIR &&
        (open_mode & FILE_OPEN_WRITE)) ||
        (type == INODE_TYPE_DIVICE &&
        !device_open_check(major, open_mode))) {
        inode_put(ip);
        return NULL;
    }

    file_t *file = file_alloc();

    if (file == NULL) {
        inode_put(ip);
        return NULL;
    }

    file->ip = ip;
    file->readable =
        (open_mode & FILE_OPEN_READ) != 0;
    file->writbale =
        (open_mode & FILE_OPEN_WRITE) != 0;
    file->offset = 0;

    return file;
}

/* 关闭文件 */
void file_close(file_t *file)
{
    assert(file != NULL,
        "file_close: file is NULL");

    inode_t *ip = NULL;

    spinlock_acquire(&lk_file_table);

    assert(file->ref > 0,
        "file_close: invalid ref");

    file->ref--;

    /*
     * 最后一个file引用关闭。
     */
    if (file->ref == 0) {
        ip = file->ip;

        file->ip = NULL;
        file->readable = false;
        file->writbale = false;
        file->offset = 0;
    }

    spinlock_release(&lk_file_table);

    /*
     * inode_put可能访问磁盘、获取睡眠锁，
     * 不能在持有file_table自旋锁时执行。
     */
    if (ip != NULL)
        inode_put(ip);
}

/* 读取文件内容, 返回读到的字节数量 */
uint32 file_read(file_t* file, uint32 len, uint64 dst, bool is_user_dst)
{
    if (file == NULL ||
        file->ip == NULL ||
        !file->readable ||
        (len > 0 && dst == 0)) {
        return 0;
    }

    inode_t *ip = file->ip;
    uint32 read_len;

    inode_lock(ip);

    switch (ip->disk_info.type) {
    case INODE_TYPE_DATA:
        read_len = inode_read_data(
            ip,
            file->offset,
            len,
            (void *)dst,
            is_user_dst
        );
        break;

    case INODE_TYPE_DIR:
        read_len = dentry_transmit(
            ip,
            dst,
            len,
            is_user_dst
        );
        break;

    case INODE_TYPE_DIVICE:
        read_len = device_read_data(
            ip->disk_info.major,
            len,
            dst,
            is_user_dst
        );
        break;

    default:
        read_len = 0;
        break;
    }

    inode_unlock(ip);

    file->offset += read_len;
    return read_len;
}

/* 写入文件内容, 返回写入的字节数量 */
uint32 file_write(file_t* file, uint32 len, uint64 src, bool is_user_src)
{
    if (file == NULL ||
        file->ip == NULL ||
        !file->writbale ||
        (len > 0 && src == 0)) {
        return 0;
    }

    inode_t *ip = file->ip;
    uint32 written;

    inode_lock(ip);

    switch (ip->disk_info.type) {
    case INODE_TYPE_DATA:
        written = inode_write_data(
            ip,
            file->offset,
            len,
            (void *)src,
            is_user_src
        );
        break;

    case INODE_TYPE_DIR:
        /*
         * 目录不能通过普通写接口修改。
         */
        written = 0;
        break;

    case INODE_TYPE_DIVICE:
        written = device_write_data(
            ip->disk_info.major,
            len,
            src,
            is_user_src
        );
        break;

    default:
        written = 0;
        break;
    }

    inode_unlock(ip);

    file->offset += written;
    return written;
}

/* 
	读/写指针的移动
	对于不合理的lseek_offset, 只做尽力而为的移动
	返回新的file->offset
*/
uint32 file_lseek(file_t *file, uint32 lseek_offset, uint32 lseek_flag)
{
    if (file == NULL ||
        file->ip == NULL) {
        return (uint32)-1;
    }

    switch (lseek_flag) {
    case FILE_LSEEK_SET:
        file->offset = lseek_offset;
        break;

    case FILE_LSEEK_ADD:
        /*
         * 防止offset + lseek_offset发生无符号上溢。
         */
        if (lseek_offset >
            (uint32)-1 - file->offset) {
            file->offset = (uint32)-1;
        } else {
            file->offset += lseek_offset;
        }
        break;

    case FILE_LSEEK_SUB:
        /*
         * 防止offset - lseek_offset发生无符号下溢。
         */
        if (lseek_offset > file->offset) {
            file->offset = 0;
        } else {
            file->offset -= lseek_offset;
        }
        break;

    default:
        return (uint32)-1;
    }

    return file->offset;
}

/* file->ref++ with lock protect */
file_t* file_dup(file_t* file)
{
    assert(file != NULL,
        "file_dup: file is NULL");

    spinlock_acquire(&lk_file_table);

    assert(file->ref > 0,
        "file_dup: invalid ref");

    file->ref++;

    spinlock_release(&lk_file_table);

    return file;
}

/* 获取文件参数, 成功返回0, 失败返回-1 */
uint32 file_get_stat(file_t* file, uint64 user_dst)
{
    if (file == NULL ||
        file->ip == NULL ||
        user_dst == 0) {
        return (uint32)-1;
    }

    file_stat_t stat;
    inode_t *ip = file->ip;

    inode_lock(ip);

    stat.type = ip->disk_info.type;
    stat.nlink = ip->disk_info.nlink;
    stat.size = ip->disk_info.size;
    stat.inode_num = ip->inode_num;

    inode_unlock(ip);

    /*
     * offset属于当前file，而不是inode。
     */
    stat.offset = file->offset;

    proc_t *p = myproc();

    if (p == NULL)
        return (uint32)-1;

    /*
     * user_dst是用户虚拟地址，不能直接memmove。
     */
    uvm_copyout(
        p->pgtbl,
        user_dst,
        (uint64)&stat,
        sizeof(stat)
    );

    return 0;
}

/* 基于superblock输出磁盘布局信息 (for debug) */
static void sb_print()
{
	printf("\ndisk layout information:\n");
	printf("1. super block:  block[0]\n");
	printf("2. inode bitmap: block[%d - %d]\n", sb.inode_bitmap_firstblock,
		sb.inode_bitmap_firstblock + sb.inode_bitmap_blocks - 1);
	printf("3. inode region: block[%d - %d]\n", sb.inode_firstblock,
		sb.inode_firstblock + sb.inode_blocks - 1);
	printf("4. data bitmap:  block[%d - %d]\n", sb.data_bitmap_firstblock,
		sb.data_bitmap_firstblock + sb.data_bitmap_blocks - 1);
	printf("5. data region:  block[%d - %d]\n", sb.data_firstblock,
		sb.data_firstblock + sb.data_blocks - 1);
	printf("block size = %d Byte, total size = %d MB, total inode = %d\n\n", sb.block_size,
		(int)((unsigned long long)(sb.total_blocks) * sb.block_size / 1024 / 1024), sb.total_inodes);
}

/* 文件系统初始化 */
void fs_init()
{
	buffer_init();

	buffer_t *buf = buffer_get(FS_SB_BLOCK);

	memmove(&sb, buf->data, sizeof(sb));

	buffer_put(buf);

	assert(sb.magic_num == FS_MAGIC,
		"fs_init: invalid superblock");
	assert(sb.block_size == BLOCK_SIZE,
		"fs_init: invalid block size");

	inode_init();
    file_init();
    device_init();

    sb_print();
}
