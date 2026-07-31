#include "mod.h"

extern super_block_t sb;

/* 内存中的inode资源集合 */
static inode_t inode_cache[N_INODE];
static spinlock_t lk_inode_cache;

/* inode_cache初始化 */
void inode_init()
{
	spinlock_init(&lk_inode_cache, "inode_cache");

    for (int i = 0; i < N_INODE; i++) {
        inode_cache[i].valid_info = false;
        inode_cache[i].inode_num = INVALID_INODE_NUM;
        inode_cache[i].ref = 0;
        sleeplock_init(&inode_cache[i].slk, "inode");
    }
}

/*--------------------关于inode->index的增删查操作-----------------*/

/*
	供free_data_blocks使用
	递归删除inode->index中的一个元素
	返回删除过程中是否遇到空的block_num (文件末尾)
*/
static bool __free_data_blocks(uint32 block_num, uint32 level)
{
	/* 0 表示文件已经没有后续块 */
    if (block_num == 0)
        return true;

    /* 递归终点：释放真正的数据块 */
    if (level == 0) {
        bitmap_free_block(block_num);
        return false;
    }

    /* 当前块是索引块，读取其中保存的下一层块号 */
    buffer_t *buf = buffer_get(block_num);
    uint32 *index = (uint32 *)buf->data;
    bool meet_empty = false;

    for (uint32 i = 0; i < BLOCK_SIZE / sizeof(uint32); i++) {
        meet_empty = __free_data_blocks(index[i], level - 1);

        if (meet_empty)
            break;
    }

    /*
     * 先归还缓存，再释放当前索引块本身。
     * 索引块同样来自 data 区，也占用 bitmap。
     */
    buffer_put(buf);
    bitmap_free_block(block_num);

    return meet_empty;
}

/*
	释放inode管理的blocks
*/
static void free_data_blocks(uint32 *inode_index)
{
	unsigned int i;
	bool meet_empty = false;

	/* step-1: 释放直接映射的block */
	for (i = 0; i < INODE_INDEX_1; i++)
	{
		meet_empty = __free_data_blocks(inode_index[i], 0);
		if (meet_empty) return;
	}

	/* step-2: 释放一级间接映射的block */
	for (; i < INODE_INDEX_2; i++)
	{
		meet_empty = __free_data_blocks(inode_index[i], 1);
		if (meet_empty) return;
	}

	/* step-3: 释放二级间接映射的block */
	for (; i < INODE_INDEX_3; i++)
	{
		meet_empty = __free_data_blocks(inode_index[i], 2);
		if (meet_empty) return;
	}

	panic("free_data_blocks: impossible!");
}

/*
	获取inode第logical_block_num个block的物理序号block_num
	调用者保证输入的logical_block_num只有两种情况:
	1. 属于已经分配的区域 (返回block_num)
	2. 将已经分配出去的区域往外扩展1个block (申请block并返回block_num)
	成功返回block_num, 失败返回-1
*/

static uint32 alloc_zero_block()
{
    uint32 block = bitmap_alloc_block();
    if (block == (uint32)-1)
        return (uint32)-1;

    buffer_t *buf = buffer_get(block);
    memset(buf->data, 0, BLOCK_SIZE);
    buffer_write(buf);
    buffer_put(buf);
    return block;
}

static uint32 locate_or_add_block(uint32 *inode_index, uint32 logical_block_num)
{
	const uint32 per_block = BLOCK_SIZE / sizeof(uint32);
    uint32 root, depth;
    uint32 path[2] = {0, 0};

    if (logical_block_num < INODE_BLOCK_INDEX_1) {
        root = logical_block_num;
        depth = 0;
    } else if (logical_block_num < INODE_BLOCK_INDEX_2) {
        uint32 n = logical_block_num - INODE_BLOCK_INDEX_1;
        root = INODE_INDEX_1 + n / per_block;
        path[0] = n % per_block;
        depth = 1;
    } else if (logical_block_num < INODE_BLOCK_INDEX_3) {
        uint32 n = logical_block_num - INODE_BLOCK_INDEX_2;
        root = INODE_INDEX_2;
        path[0] = n / per_block;
        path[1] = n % per_block;
        depth = 2;
    } else {
        return (uint32)-1;
    }

    uint32 *slot = &inode_index[root];
    buffer_t *owner = NULL;

    for (uint32 level = 0; level < depth; level++) {
        if (*slot == 0) {
            uint32 block = alloc_zero_block();
            if (block == (uint32)-1) {
                if (owner != NULL)
                    buffer_put(owner);
                return (uint32)-1;
            }
            *slot = block;
            if (owner != NULL)
                buffer_write(owner);
        }

        uint32 block = *slot;
        if (owner != NULL)
            buffer_put(owner);

        owner = buffer_get(block);
        slot = &((uint32 *)owner->data)[path[level]];
    }

    if (*slot == 0) {
        uint32 block = alloc_zero_block();
        if (block == (uint32)-1) {
            if (owner != NULL)
                buffer_put(owner);
            return (uint32)-1;
        }
        *slot = block;
        if (owner != NULL)
            buffer_write(owner);
    }

    uint32 result = *slot;
    if (owner != NULL)
        buffer_put(owner);
    return result;
}

/*---------------------关于inode的管理: get dup lock unlock put----------------------*/

/*
	磁盘里的inode <-> 内存里的inode
	调用者需要持有ip->slk并设置合理的inode_num
*/
void inode_rw(inode_t *ip, bool write)
{
	assert(ip != NULL, "inode_rw: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "inode_rw: inode not locked");
    assert(ip->inode_num < sb.total_inodes,
        "inode_rw: invalid inode number");

    uint32 block_num =
        sb.inode_firstblock +
        ip->inode_num / INODE_PER_BLOCK;

    uint32 slot =
        ip->inode_num % INODE_PER_BLOCK;

    buffer_t *buf = buffer_get(block_num);
    inode_disk_t *disk_inode =
        &((inode_disk_t *)buf->data)[slot];

    if (write) {
        assert(ip->valid_info,
            "inode_rw: invalid memory inode");

        memmove(disk_inode,
            &ip->disk_info,
            sizeof(inode_disk_t));
        buffer_write(buf);
    } else {
        memmove(&ip->disk_info,
            disk_inode,
            sizeof(inode_disk_t));
        ip->valid_info = true;
    }

    buffer_put(buf);
}

/*
	尝试在inode_cache里寻找是否存在目标inode
	如果不存在则申请一个空闲的inode
	如果没有空闲位置直接panic
	核心逻辑: ref++
*/
inode_t *inode_get(uint32 inode_num)
{
	assert(inode_num < sb.total_inodes,
        "inode_get: invalid inode number");

    inode_t *empty = NULL;

    spinlock_acquire(&lk_inode_cache);

    for (int i = 0; i < N_INODE; i++) {
        inode_t *ip = &inode_cache[i];

        /* cache hit：同一个磁盘inode已经有内存副本 */
        if (ip->ref > 0 && ip->inode_num == inode_num) {
            ip->ref++;
            spinlock_release(&lk_inode_cache);
            return ip;
        }

        /* 暂时记下第一个空闲槽位，但继续寻找命中项 */
        if (empty == NULL && ip->ref == 0)
            empty = ip;
    }

    if (empty == NULL) {
        spinlock_release(&lk_inode_cache);
        panic("inode_get: no available inode");
    }

    /* cache miss：让空闲槽位代表目标磁盘inode */
    memset(&empty->disk_info, 0, sizeof(empty->disk_info));
    empty->valid_info = false;
    empty->inode_num = inode_num;
    empty->ref = 1;

    spinlock_release(&lk_inode_cache);
    return empty;
}

/*
	在磁盘里创建1个新的inode
	1. 查询和修改inode_bitmap
	2. 填充inode_region对应位置的inode
	注意: 返回的inode未上锁
*/
inode_t *inode_create(uint16 type, uint16 major, uint16 minor)
{
    assert(type <= INODE_TYPE_DIVICE,
        "inode_create: invalid inode type");

    uint32 inode_num = bitmap_alloc_inode();
    if (inode_num == (uint32)-1)
        return NULL;

    /*
     * inode_get使缓存中出现该inode：
     * inode_num已设置、ref=1、valid_info=false。
     */
    inode_t *ip = inode_get(inode_num);
    inode_lock(ip);

    /* 清除磁盘槽位中可能遗留的旧inode信息 */
    memset(&ip->disk_info, 0, sizeof(ip->disk_info));

    ip->disk_info.type = type;
    ip->disk_info.major = major;
    ip->disk_info.minor = minor;
    ip->disk_info.nlink = 1;
    ip->disk_info.size = 0;
    ip->valid_info = true;

    inode_rw(ip, true);
    inode_unlock(ip);

    return ip;
}

/*
	ip->ref++ with lock proctect
*/
inode_t* inode_dup(inode_t* ip)
{
	assert(ip != NULL, "inode_dup: inode is NULL");

    spinlock_acquire(&lk_inode_cache);
    assert(ip->ref > 0, "inode_dup: invalid ref");
    ip->ref++;
    spinlock_release(&lk_inode_cache);

    return ip;
}

/*
	锁住inode
	如果inode->disk_info无效则更新一波
*/
void inode_lock(inode_t* ip)
{
	assert(ip != NULL, "inode_lock: inode is NULL");

    spinlock_acquire(&lk_inode_cache);
    bool referenced = ip->ref > 0;
    spinlock_release(&lk_inode_cache);

    assert(referenced, "inode_lock: invalid ref");

    sleeplock_acquire(&ip->slk);

    /* cache miss时延迟从磁盘加载 */
    if (!ip->valid_info)
        inode_rw(ip, false);
}

/*
	解锁inode
*/
void inode_unlock(inode_t *ip)
{
	assert(ip != NULL, "inode_unlock: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "inode_unlock: inode not locked");

    sleeplock_release(&ip->slk);
}

/*
	与inode_get相对应, 调用者释放inode资源
	如果达成某些条件, 可能触发彻底删除
*/
void inode_put(inode_t* ip)
{
    assert(ip != NULL, "inode_put: inode is NULL");
    assert(!sleeplock_holding(&ip->slk),
        "inode_put: inode still locked");

    spinlock_acquire(&lk_inode_cache);
    assert(ip->ref > 0, "inode_put: invalid ref");

    /*
     * 这是最后一个内存引用，并且该inode已没有目录链接，
     * 因此需要同时删除磁盘资源。
     */
    if (ip->ref == 1 &&
        ip->valid_info &&
        ip->disk_info.nlink == 0) {

        /*
         * ref == 1保证没有其他合法使用者持有该inode锁，
         * 因此这里取得睡眠锁不会等待。
         */
        sleeplock_acquire(&ip->slk);

        /*
         * inode_delete会访问bitmap和磁盘，可能导致进程睡眠，
         * 不能在持有全局自旋锁时执行。
         */
        spinlock_release(&lk_inode_cache);

        inode_delete(ip);

        sleeplock_release(&ip->slk);

        spinlock_acquire(&lk_inode_cache);
    }

    ip->ref--;

    /* ref归零后，该inode_cache槽位可以复用 */
    if (ip->ref == 0) {
        ip->valid_info = false;
        ip->inode_num = INVALID_INODE_NUM;
    }

    spinlock_release(&lk_inode_cache);
}

/*
	在磁盘里删除1个inode
	1. 修改inode_bitmap释放inode_region资源
	2. 修改block_bitmap释放block_region资源
	注意: 调用者需要持有ip->slk
*/
void inode_delete(inode_t *ip)
{
    assert(ip != NULL, "inode_delete: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "inode_delete: inode not locked");
    assert(ip->valid_info,
        "inode_delete: invalid inode");
    assert(ip->disk_info.nlink == 0,
        "inode_delete: inode still linked");

    /*
     * 释放普通数据块、一级索引块和二级索引块。
     * free_data_blocks会通过data bitmap归还它们。
     */
    free_data_blocks(ip->disk_info.index);

    /*
     * 清空内存中的磁盘格式信息，并把清零后的inode
     * 写回inode region，避免磁盘槽位残留旧信息。
     */
    memset(&ip->disk_info, 0, sizeof(ip->disk_info));
    inode_rw(ip, true);

    /* inode磁盘槽位清理完成后，归还inode编号 */
    bitmap_free_inode(ip->inode_num);

    /*
     * 当前inode_t仍位于静态inode_cache中，
     * 但它已不再代表有效的磁盘inode。
     */
    ip->valid_info = false;
}

/*----------------------基于inode的数据读写操作--------------------*/

/*
	基于inode的数据读取
	inode管理的数据空间逻辑上是一个连续的数组data
	需要拷贝data[offset,offset+len)到dst(用户态地址/内核态地址)
	返回读取的数据量(字节)
*/
uint32 inode_read_data(inode_t *ip, uint32 offset, uint32 len, void *dst, bool is_user_dst)
{
    assert(ip != NULL, "inode_read_data: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "inode_read_data: inode not locked");
    assert(ip->valid_info,
        "inode_read_data: invalid inode");
    assert(ip->disk_info.type != INODE_TYPE_DIVICE,
        "inode_read_data: device inode");

    if (len == 0 || offset >= ip->disk_info.size)
        return 0;

    assert(dst != NULL, "inode_read_data: dst is NULL");

    /* 读取不能超过文件末尾，同时避免offset + len溢出 */
    uint32 available = ip->disk_info.size - offset;
    if (len > available)
        len = available;

    proc_t *p = NULL;
    if (is_user_dst) {
        p = myproc();
        assert(p != NULL, "inode_read_data: no process");
    }

    uint32 copied = 0;

    while (copied < len) {
        uint32 position = offset + copied;
        uint32 logical_block = position / BLOCK_SIZE;
        uint32 block_offset = position % BLOCK_SIZE;
        uint32 cut_len =
            MIN(len - copied, BLOCK_SIZE - block_offset);

        uint32 block_num = locate_or_add_block(
            ip->disk_info.index, logical_block);

        if (block_num == (uint32)-1)
            break;

        buffer_t *buf = buffer_get(block_num);
        uint64 src = (uint64)(buf->data + block_offset);

        if (is_user_dst) {
            uvm_copyout(p->pgtbl,
                (uint64)dst + copied, src, cut_len);
        } else {
            memmove((uint8 *)dst + copied,
                (void *)src, cut_len);
        }

        buffer_put(buf);
        copied += cut_len;
    }

    return copied;
}

/*
	基于inode的数据写入
	inode管理的数据空间逻辑上是一个连续的数组data
	需要拷贝src(用户态地址/内核态地址)到data[offset,offset+len)
	返回写入的数据量(字节)
*/
uint32 inode_write_data(inode_t *ip, uint32 offset, uint32 len, void *src, bool is_user_src)
{
    assert(ip != NULL, "inode_write_data: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "inode_write_data: inode not locked");
    assert(ip->valid_info,
        "inode_write_data: invalid inode");
    assert(ip->disk_info.type != INODE_TYPE_DIVICE,
        "inode_write_data: device inode");

    if (len == 0)
        return 0;

    assert(src != NULL, "inode_write_data: src is NULL");

    /* 普通文件不允许在文件末尾之后留下空洞 */
    if (ip->disk_info.type == INODE_TYPE_DATA &&
        offset > ip->disk_info.size)
        return 0;

    /* 本实验中的目录最多占用一个block */
    uint32 limit = ip->disk_info.type == INODE_TYPE_DIR
        ? BLOCK_SIZE
        : INODE_MAX_SIZE;

    if (offset >= limit)
        return 0;

    if (len > limit - offset)
        len = limit - offset;

    proc_t *p = NULL;
    if (is_user_src) {
        p = myproc();
        assert(p != NULL, "inode_write_data: no process");
    }

    uint32 written = 0;

    while (written < len) {
        uint32 position = offset + written;
        uint32 logical_block = position / BLOCK_SIZE;
        uint32 block_offset = position % BLOCK_SIZE;
        uint32 cut_len =
            MIN(len - written, BLOCK_SIZE - block_offset);

        uint32 block_num = locate_or_add_block(
            ip->disk_info.index, logical_block);

        if (block_num == (uint32)-1)
            break;

        buffer_t *buf = buffer_get(block_num);
        uint64 dst = (uint64)(buf->data + block_offset);

        if (is_user_src) {
            uvm_copyin(p->pgtbl, dst,
                (uint64)src + written, cut_len);
        } else {
            memmove((void *)dst,
                (uint8 *)src + written, cut_len);
        }

        buffer_write(buf);
        buffer_put(buf);
        written += cut_len;
    }

    /* 普通文件的size表示连续有效区间[0, size) */
    if (ip->disk_info.type == INODE_TYPE_DATA) {
        uint32 end = offset + written;
        if (end > ip->disk_info.size)
            ip->disk_info.size = end;
    }

    /*
     * 持久化size和index[]。即使分配中途失败，
     * locate_or_add_block也可能已经创建了中间索引块。
     */
    inode_rw(ip, true);

    return written;
}

static char *inode_type_list[] = {"DATA", "DIR", "DEVICE"};

/* 输出inode信息(for debug) */
void inode_print(inode_t *ip, char* name)
{
	assert(sleeplock_holding(&ip->slk), "inode_print: slk");

	spinlock_acquire(&lk_inode_cache);

	printf("inode %s:\n", name);
	printf("ref = %d, inode_num = %d, valid_info = %d\n", ip->ref, ip->inode_num, ip->valid_info);
	printf("type = %s, major = %d, minor = %d, nlink = %d, size = %d\n", inode_type_list[ip->disk_info.type],
		ip->disk_info.major, ip->disk_info.minor, ip->disk_info.nlink, ip->disk_info.size);

	printf("index_list = [ ");
	for (int i = 0; i < INODE_INDEX_1; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("] [ ");
	for (int i = INODE_INDEX_1; i < INODE_INDEX_2; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("] [ ");
	for (int i = INODE_INDEX_2; i < INODE_INDEX_3; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("]\n\n");

	spinlock_release(&lk_inode_cache);
}
