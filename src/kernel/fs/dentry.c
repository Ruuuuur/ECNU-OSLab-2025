#include "mod.h"

/*
	出于简化目的的假设:
	如果inode_disk.type == INODE_TYPE_DIR
	那么inode_disk.size <= BLOCKSIZE (只有inode_disk.index[0]有效)
	也就是说, 单个目录最多包含BLOCKSIZE / sizeof(dentry)个目录项

	另外, INODE_TYPE_DATA要求数据之间没有空隙
	但是对于INODE_TYPE_DIR来说是无法做到的(目录项的删除很常见)
	因此, ip->size代表block中已经使用的空间大小
*/


/*----------------dentry的查找、增加、删除操作-----------------*/

/*
	在目录ip中查找是否存在名字为name的目录项
	如果找到了返回目录项中存储的inode_num
	如果没找到返回INVALID_INODE_NUM
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_search(inode_t *ip, char *name)
{
	assert(ip != NULL, "dentry_search: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "dentry_search: inode not locked");
    assert(ip->valid_info,
        "dentry_search: invalid inode");
    assert(ip->disk_info.type == INODE_TYPE_DIR,
        "dentry_search: inode is not directory");
    assert(name != NULL, "dentry_search: name is NULL");

    if (name[0] == '\0' ||
        strlen(name) >= MAXLEN_FILENAME)
        return INVALID_INODE_NUM;

    /* 空目录尚未分配目录数据块 */
    if (ip->disk_info.size == 0)
        return INVALID_INODE_NUM;

    assert(ip->disk_info.index[0] != 0,
        "dentry_search: directory block missing");

    buffer_t *buf =
        buffer_get(ip->disk_info.index[0]);

    dentry_t *entries = (dentry_t *)buf->data;

    for (uint32 i = 0; i < DENTRY_PER_BLOCK; i++) {
        dentry_t *de = &entries[i];

        /* name[0] == 0表示空闲目录项槽位 */
        if (de->name[0] == '\0')
            continue;

        if (strncmp(de->name, name,
                    MAXLEN_FILENAME) == 0) {
            uint32 inode_num = de->inode_num;
            buffer_put(buf);
            return inode_num;
        }
    }

    buffer_put(buf);
    return INVALID_INODE_NUM;
}

/*
	在目录ip中查找是否存在序号为inode_num的目录项
	如果存在则将它的名字拷贝到name, 返回name_len
	如果不存在则返回-1
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_search_2(inode_t *ip, uint32 inode_num, char *name)
{
    assert(ip != NULL,
        "dentry_search_2: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "dentry_search_2: inode not locked");
    assert(ip->valid_info,
        "dentry_search_2: invalid inode");
    assert(ip->disk_info.type == INODE_TYPE_DIR,
        "dentry_search_2: inode is not directory");
    assert(name != NULL,
        "dentry_search_2: name is NULL");

    if (ip->disk_info.size == 0)
        return (uint32)-1;

    assert(ip->disk_info.index[0] != 0,
        "dentry_search_2: directory block missing");

    buffer_t *buf = buffer_get(ip->disk_info.index[0]);
    dentry_t *entries = (dentry_t *)buf->data;

    for (uint32 i = 0; i < DENTRY_PER_BLOCK; i++) {
        dentry_t *de = &entries[i];

        if (de->name[0] == '\0')
            continue;

        if (de->inode_num == inode_num) {
            uint32 name_len = strlen(de->name);

            memmove(name, de->name, name_len + 1);
            buffer_put(buf);
            return name_len;
        }
    }

    buffer_put(buf);
    return (uint32)-1;
}

/*
	在目录ip中寻找空闲槽位, 插入新的dentry
	如果成功插入则返回这个目录项的偏移量(还需要更新size)
	如果插入失败(没有空间/发生重名)返回-1
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_create(inode_t *ip, uint32 inode_num, char *name)
{
	assert(ip != NULL, "dentry_create: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "dentry_create: inode not locked");
    assert(ip->valid_info,
        "dentry_create: invalid inode");
    assert(ip->disk_info.type == INODE_TYPE_DIR,
        "dentry_create: inode is not directory");
    assert(name != NULL, "dentry_create: name is NULL");

    uint32 name_len = strlen(name);
    if (name_len == 0 ||
        name_len >= MAXLEN_FILENAME ||
        inode_num == INVALID_INODE_NUM)
        return (uint32)-1;

    assert(ip->disk_info.size % sizeof(dentry_t) == 0,
        "dentry_create: invalid directory size");

    /* size表示有效目录项占用量 */
    if (ip->disk_info.size >
        BLOCK_SIZE - sizeof(dentry_t))
        return (uint32)-1;

    buffer_t *buf;

    if (ip->disk_info.index[0] == 0) {
        uint32 block_num = bitmap_alloc_block();
        if (block_num == (uint32)-1)
            return (uint32)-1;

        ip->disk_info.index[0] = block_num;
        buf = buffer_get(block_num);

        /* 新分配的磁盘块可能保留旧内容 */
        memset(buf->data, 0, BLOCK_SIZE);
    } else {
        buf = buffer_get(ip->disk_info.index[0]);
    }

    dentry_t *entries = (dentry_t *)buf->data;
    dentry_t *empty = NULL;

    for (uint32 i = 0; i < DENTRY_PER_BLOCK; i++) {
        dentry_t *de = &entries[i];

        if (de->name[0] == '\0') {
            if (empty == NULL)
                empty = de;
            continue;
        }

        if (strncmp(de->name, name,
                    MAXLEN_FILENAME) == 0) {
            buffer_put(buf);
            return (uint32)-1;
        }
    }

    if (empty == NULL) {
        buffer_put(buf);
        return (uint32)-1;
    }

    memset(empty, 0, sizeof(*empty));
    memmove(empty->name, name, name_len);
    empty->inode_num = inode_num;

    uint32 offset =
        (uint32)((uint8 *)empty - buf->data);

    buffer_write(buf);
    buffer_put(buf);

    ip->disk_info.size += sizeof(dentry_t);
    inode_rw(ip, true);

    return offset;
}

/*
	在目录ip下删除名称为name的dentry, 返回它的inode_num
	如果匹配失败或者遇到非法情况返回INVALID_INODE_NUM
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_delete(inode_t *ip, char *name)
{
	assert(ip != NULL, "dentry_delete: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "dentry_delete: inode not locked");
    assert(ip->valid_info,
        "dentry_delete: invalid inode");
    assert(ip->disk_info.type == INODE_TYPE_DIR,
        "dentry_delete: inode is not directory");
    assert(name != NULL, "dentry_delete: name is NULL");

    uint32 name_len = strlen(name);
    if (name_len == 0 || name_len >= MAXLEN_FILENAME)
        return INVALID_INODE_NUM;

    /* 本实验中不允许删除特殊目录项 */
    if ((name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' &&
        name[2] == '\0'))
        return INVALID_INODE_NUM;

    assert(ip->disk_info.size <= BLOCK_SIZE &&
        ip->disk_info.size % sizeof(dentry_t) == 0,
        "dentry_delete: invalid directory size");

    if (ip->disk_info.size == 0)
        return INVALID_INODE_NUM;

    assert(ip->disk_info.index[0] != 0,
        "dentry_delete: directory block missing");

    buffer_t *buf =
        buffer_get(ip->disk_info.index[0]);
    dentry_t *entries = (dentry_t *)buf->data;

    for (uint32 i = 0; i < DENTRY_PER_BLOCK; i++) {
        dentry_t *de = &entries[i];

        if (de->name[0] == '\0')
            continue;

        if (strncmp(de->name, name,
                    MAXLEN_FILENAME) == 0) {
            uint32 inode_num = de->inode_num;

            /* name[0]=0之后，该槽位可被再次使用 */
            memset(de, 0, sizeof(*de));
            buffer_write(buf);
            buffer_put(buf);

            ip->disk_info.size -= sizeof(dentry_t);
            inode_rw(ip, true);

            return inode_num;
        }
    }

    buffer_put(buf);
    return INVALID_INODE_NUM;
}

/*
	向缓冲区[dst, dst + len)中填充有效的dentry
	返回成功填充的数据量(字节)
	注意: 调用者需持有ip->slk
*/
uint32 dentry_transmit(inode_t *ip, uint64 dst, uint32 len, bool is_user_dst)
{
    assert(ip != NULL,
        "dentry_transmit: inode is NULL");
    assert(sleeplock_holding(&ip->slk),
        "dentry_transmit: inode not locked");
    assert(ip->valid_info,
        "dentry_transmit: invalid inode");
    assert(ip->disk_info.type == INODE_TYPE_DIR,
        "dentry_transmit: inode is not directory");

    if (len < sizeof(dentry_t) ||
        ip->disk_info.size == 0)
        return 0;

    assert(ip->disk_info.index[0] != 0,
        "dentry_transmit: directory block missing");

    proc_t *p = NULL;
    if (is_user_dst) {
        p = myproc();
        assert(p != NULL,
            "dentry_transmit: no current process");
    }

    buffer_t *buf =
        buffer_get(ip->disk_info.index[0]);
    dentry_t *entries = (dentry_t *)buf->data;
    uint32 copied = 0;

    for (uint32 i = 0;
        i < DENTRY_PER_BLOCK &&
        copied + sizeof(dentry_t) <= len;
        i++) {
        dentry_t *de = &entries[i];

        if (de->name[0] == '\0')
            continue;

        if (is_user_dst) {
            uvm_copyout(
                p->pgtbl,
                dst + copied,
                (uint64)de,
                sizeof(dentry_t)
            );
        } else {
            memmove(
                (uint8 *)dst + copied,
                de,
                sizeof(dentry_t)
            );
        }

        copied += sizeof(dentry_t);
    }

    buffer_put(buf);
    return copied;
}

/* 输出目录中所有有效目录项的信息 (for debug) */
void dentry_print(inode_t *ip)
{
	assert(sleeplock_holding(&ip->slk), "dentry_print: slk!");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_print: not dir!");

	dentry_t *de;
	buffer_t *buf;

	if (ip->disk_info.index[0] == 0)
		panic("dentry_print: invalid index[0]!");

	printf("inode_num = %d, dentries:\n", ip->inode_num);

	buf = buffer_get(ip->disk_info.index[0]);
	for (de = (dentry_t*)(buf->data); de < (dentry_t*)(buf->data + BLOCK_SIZE); de++)
	{
		if (de->name[0] != 0) {
			printf("dentry: offset = %d, inode_num = %d, name = %s\n",
				(uint32)((uint8*)de - buf->data), de->inode_num, de->name);
		}
	}
	buffer_put(buf);

	printf("\n");
}

/*------------------从文件名到文件路径-----------------*/

/*
	Examples:
	get_element("a/bb/c", name) = "bb/c" + name = "a"
	get_element("///aa//bb", name) = "bb" + name = "aa"
	get_element("aaa", name) = "" + name = "aaa"
	get_element("", name) = NULL + name = ""
	get_element("//", name) = NULL + name = ""
*/
static char* get_element(char *path, char *name)
{
	/* 跳过前置的'/' */
    while (*path == '/')
		path++;

	/* 如果遇到末尾了则返回 */
    if (*path == 0) {
		name[0] = 0;
		return NULL;
	}

	/* 记录起点位置 */
    char *start = path;

	/* 推进path直到遇到'/'或者到达末尾 */
	while (*path != '/' && *path != 0)
        path++;

	/* 提取到的name的长度 */
    int len = path - start;
	len = MIN(len, MAXLEN_FILENAME-1);

	/* 设置name */
	memmove(name, start, len);
	name[len] = 0;

	/* 跳过后置的'/' */
    while (*path == '/') path++;

    return path;
}
/*
	根据文件路径(/A/B/C)查找对应inode(inode_B or inode_C)
	如果find_parent_inode == true, 返回父节点inode, name为下一级子节点的名字
	如果find_parent_inode == false, 返回子节点inode, name无意义
	如果失败返回NULL
*/
static inode_t* __path_to_inode(char *path, char *name, bool find_parent_inode)
{
    if (path == NULL ||
        name == NULL ||
        path[0] == '\0')
        return NULL;

    inode_t *ip;

    /*
    * 绝对路径从根目录开始，
    * 相对路径从当前进程的工作目录开始。
    */
    if (path[0] == '/') {
        ip = inode_get(ROOT_INODE);
    } else {
        proc_t *p = myproc();

        if (p == NULL || p->cwd == NULL)
            return NULL;

        ip = inode_dup(p->cwd);
    }

    char *rest = path;

    while ((rest = get_element(rest, name)) != NULL) {
        inode_lock(ip);

        /* 只有目录才能继续查找下一级名称 */
        if (ip->disk_info.type != INODE_TYPE_DIR) {
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }

        /*
         * 已取出路径最后一个名称。
         * 查找父目录时，当前ip就是所需父节点。
         */
        if (find_parent_inode && *rest == '\0') {
            inode_unlock(ip);
            return ip;
        }

        uint32 inode_num = dentry_search(ip, name);
        inode_unlock(ip);

        if (inode_num == INVALID_INODE_NUM) {
            inode_put(ip);
            return NULL;
        }

        /*
         * 先取得下一级引用，再归还当前引用。
         * 对"."或".."指向自身的情况同样安全。
         */
        inode_t *next = inode_get(inode_num);
        inode_put(ip);
        ip = next;
    }

    /*
     * "/"没有最后一级名称：
     * path_to_inode("/")返回根inode，
     * path_to_parent_inode("/")没有合法父目标。
     */
    if (find_parent_inode) {
        inode_put(ip);
        return NULL;
    }

    return ip;
}

/*
	基于path寻找inode
	失败返回NULL
*/
inode_t* path_to_inode(char *path)
{
	char name[MAXLEN_FILENAME];
	return __path_to_inode(path, name, false);
}

/*
	基于path寻找inode->parent, 将inode->name放入name
	失败返回NULL, 同时name无效
*/
inode_t* path_to_parent_inode(char *path, char *name)
{
	return __path_to_inode(path, name, true);
}

/*
	将inode对应的完整路径填入path中(缓冲区长度为len)
	成功返回偏移量(从path+offset开始有效), 失败返回-1
*/
uint32 inode_to_path(inode_t *ip, char *path, uint32 len)
{
    assert(ip != NULL,
        "inode_to_path: inode is NULL");
    assert(path != NULL,
        "inode_to_path: path is NULL");

    if (len < 2)
        return (uint32)-1;

    inode_t *cur = inode_dup(ip);
    uint32 pos = len - 1;
    path[pos] = '\0';

    inode_lock(cur);
    bool is_dir = cur->disk_info.type == INODE_TYPE_DIR;
    uint32 cur_num = cur->inode_num;
    inode_unlock(cur);

    if (!is_dir) {
        inode_put(cur);
        return (uint32)-1;
    }

    /* 当前就是根目录 */
    if (cur_num == ROOT_INODE) {
        path[--pos] = '/';
        inode_put(cur);
        return pos;
    }

    char component[MAXLEN_FILENAME];

    while (cur_num != ROOT_INODE) {
        uint32 parent_num;

        /* 通过当前目录的 .. 找父目录 */
        inode_lock(cur);
        parent_num = dentry_search(cur, "..");
        inode_unlock(cur);

        if (parent_num == INVALID_INODE_NUM) {
            inode_put(cur);
            return (uint32)-1;
        }

        inode_t *parent = inode_get(parent_num);

        /* 在父目录中通过 inode 号反查当前目录名 */
        inode_lock(parent);
        uint32 name_len =
            dentry_search_2(parent, cur_num, component);
        inode_unlock(parent);

        if (name_len == (uint32)-1 ||
            name_len + 1 > pos) {
            inode_put(parent);
            inode_put(cur);
            return (uint32)-1;
        }

        /* 从缓冲区末尾向前写入：/name */
        pos -= name_len;
        memmove(path + pos, component, name_len);
        path[--pos] = '/';

        inode_put(cur);
        cur = parent;
        cur_num = parent_num;
    }

    inode_put(cur);
    return pos;
}

/*
	基于path创建新的inode
	成功返回inode, 失败返回NULL
*/
inode_t* path_create_inode(char *path, uint16 type, uint16 major, uint16 minor)
{
    if (path == NULL || path[0] == '\0')
        return NULL;

    if (type > INODE_TYPE_DIVICE)
        return NULL;

    char name[MAXLEN_FILENAME];
    inode_t *parent = path_to_parent_inode(path, name);

    if (parent == NULL ||
        name[0] == '\0' ||
        (name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' &&
        name[2] == '\0')) {
        if (parent != NULL)
            inode_put(parent);
        return NULL;
    }

    /* 不能覆盖已有目录项 */
    inode_lock(parent);
    bool exists =
        dentry_search(parent, name) != INVALID_INODE_NUM;
    inode_unlock(parent);

    if (exists) {
        inode_put(parent);
        return NULL;
    }

    /* 申请并初始化 inode */
    inode_t *ip =
        inode_create(type, major, minor);

    if (ip == NULL) {
        inode_put(parent);
        return NULL;
    }

    bool success = true;

    /*
     * 目录必须自带 . 和 .. 两个目录项。
     */
    if (type == INODE_TYPE_DIR) {
        inode_lock(ip);

        if (dentry_create(
                ip, ip->inode_num, ".") ==
                (uint32)-1 ||
            dentry_create(
                ip, parent->inode_num, "..") ==
                (uint32)-1) {
            success = false;
        }

        inode_unlock(ip);
    }

    /*
     * 把新 inode 挂到父目录下。
     */
    if (success) {
        inode_lock(parent);

        if (dentry_create(
                parent, ip->inode_num, name) ==
                (uint32)-1) {
            success = false;
        }

        inode_unlock(parent);
    }

    inode_put(parent);

    /*
     * 创建失败时，让 inode_put() 负责最终回收
     * inode、数据块和 bitmap。
     */
    if (!success) {
        inode_lock(ip);
        ip->disk_info.nlink = 0;
        inode_rw(ip, true);
        inode_unlock(ip);

        inode_put(ip);
        return NULL;
    }

    return ip;
}

/*
	构建文件硬链接 (new_path 指向 old_path 指向的 inode)
	核心操作包括 nlink++ 和 dentry_create()
	注意: old_path指向的inode不能是目录类型的
	成功返回0, 失败返回-1
*/
uint32 path_link(char *old_path, char *new_path)
{
    inode_t *old_ip = path_to_inode(old_path);

    if (old_ip == NULL)
        return (uint32)-1;

    char name[MAXLEN_FILENAME];
    inode_t *parent =
        path_to_parent_inode(new_path, name);

    if (parent == NULL ||
        name[0] == '\0' ||
        (name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' &&
        name[2] == '\0')) {
        if (parent != NULL)
            inode_put(parent);

        inode_put(old_ip);
        return (uint32)-1;
    }

    /* 不允许给目录建立硬链接 */
    inode_lock(old_ip);
    bool old_is_dir =
        old_ip->disk_info.type == INODE_TYPE_DIR;
    inode_unlock(old_ip);

    if (old_is_dir) {
        inode_put(parent);
        inode_put(old_ip);
        return (uint32)-1;
    }

    /*
     * 在新路径的父目录中增加：
     * new_name -> old inode
     */
    inode_lock(parent);

    bool exists =
        dentry_search(parent, name) !=
        INVALID_INODE_NUM;

    uint32 result = exists
        ? (uint32)-1
        : dentry_create(
            parent, old_ip->inode_num, name);

    inode_unlock(parent);

    if (result == (uint32)-1) {
        inode_put(parent);
        inode_put(old_ip);
        return (uint32)-1;
    }

    /* 持久化增加硬链接数 */
    inode_lock(old_ip);
    old_ip->disk_info.nlink++;
    inode_rw(old_ip, true);
    inode_unlock(old_ip);

    inode_put(parent);
    inode_put(old_ip);

    return 0;
}

/*
	解除文件硬链接
	成功返回0, 失败返回-1
*/
uint32 path_unlink(char *path)
{
    char name[MAXLEN_FILENAME];
    inode_t *parent =
        path_to_parent_inode(path, name);

    if (parent == NULL ||
        name[0] == '\0' ||
        (name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' &&
        name[2] == '\0')) {
        if (parent != NULL)
            inode_put(parent);

        return (uint32)-1;
    }

    inode_lock(parent);

    uint32 inode_num =
        dentry_search(parent, name);

    if (inode_num == INVALID_INODE_NUM ||
        inode_num == ROOT_INODE) {
        inode_unlock(parent);
        inode_put(parent);
        return (uint32)-1;
    }

    inode_t *ip = inode_get(inode_num);
    inode_lock(ip);

    /*
     * 普通文件可以直接解除链接。
     * 目录只有剩下 . 和 .. 时才能删除。
     */
    bool removable =
        ip->disk_info.type != INODE_TYPE_DIR ||
        ip->disk_info.size ==
            2 * sizeof(dentry_t);

    if (!removable ||
        ip->disk_info.nlink == 0) {
        inode_unlock(ip);
        inode_unlock(parent);
        inode_put(ip);
        inode_put(parent);
        return (uint32)-1;
    }

    uint32 deleted =
        dentry_delete(parent, name);

    if (deleted == INVALID_INODE_NUM) {
        inode_unlock(ip);
        inode_unlock(parent);
        inode_put(ip);
        inode_put(parent);
        return (uint32)-1;
    }

    ip->disk_info.nlink--;
    inode_rw(ip, true);

    inode_unlock(ip);
    inode_unlock(parent);

    /*
     * 如果nlink已经变成0，inode_put可能触发
     * inode_delete，回收inode和数据块。
     */
    inode_put(ip);
    inode_put(parent);

    return 0;
}