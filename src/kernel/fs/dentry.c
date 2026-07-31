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
	if (path == NULL || name == NULL ||
        path[0] != '/')
        return NULL;

    inode_t *ip = inode_get(ROOT_INODE);
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
