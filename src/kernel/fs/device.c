#include "mod.h"

device_t device_table[N_DEVICE];

/* 标准输入设备 */
static uint32 device_stdin_read(uint32 len, uint64 dst, bool is_user_dst)
{
	return cons_read(len, dst, is_user_dst);
}

/* 标准输出设备 */
static uint32 device_stdout_write(uint32 len, uint64 src, bool is_user_src)
{
	return cons_write(len, src, is_user_src);
}

/* 标准错误输出设备 */
static uint32 device_stderr_write(uint32 len, uint64 src, bool is_user_src)
{
	printf("ERROR: ");
	return cons_write(len, src, is_user_src);
}

/* 无限0流 */
static uint32 device_zero_read(uint32 len, uint64 dst, bool is_user_dst)
{
	uint32 write_len = 0, cut_len = 0;

	uint64 src = (uint64)pmem_alloc(true);
	proc_t *p = myproc();

	while (write_len < len)
	{
		cut_len = MIN(len - write_len, PGSIZE);
		
		if (is_user_dst)
			uvm_copyout(p->pgtbl, dst, src, cut_len);
		else
			memmove((void*)dst, (void*)src, cut_len);

		dst += cut_len;
		write_len += cut_len;
	}

	pmem_free(src, true);

	return write_len;
}

/* 空设备读取 */
static uint32 device_null_read(uint32 len, uint64 dst, bool is_user_dst)
{
	return 0;
}

/* 空设备写入 */
static uint32 device_null_write(uint32 len, uint64 src, bool is_user_src)
{
	return len;
}

/* 彩蛋: 笨蛋GPT */
static uint32 device_gpt0_write(uint32 len, uint64 src, bool is_user_src)
{
	char tmp[STR_MAXLEN + 1];
	proc_t *p = myproc();

	tmp[len] = '\0';

	if (is_user_src)
		uvm_copyin(p->pgtbl, (uint64)tmp, src, len);
	else
		memmove(tmp, (void*)src, len);

	if (strncmp(tmp, "Hello", len) == 0) {
		printf("Hi, I am gpt0!\n");
	} else if (strncmp(tmp, "Guess who I am", len) == 0) {
		printf("Your procid is %d and name is %s.\n", p->pid, p->name);
	} else if (strncmp(tmp, "How many free memory left", len) == 0) {
		uint32 kernel_free_pages, user_free_pages;
		pmem_stat(&kernel_free_pages, &user_free_pages);
		printf("We have %d free pages in kernel space, %d free pages in user space!\n",
			kernel_free_pages, user_free_pages);
	} else if (strncmp(tmp, "Good job", len) == 0) {
		printf("Thanks for your kind words!\n");
	} else {
		printf("Sorry, I can not understand it.\n");
	}

	return len;
}

/* 注册设备 */
static void device_register(uint32 index, char* name,
	uint32(*read)(uint32, uint64, bool),
	uint32(*write)(uint32, uint64, bool))
{
	memmove(device_table[index].name, name, MAXLEN_FILENAME);
	device_table[index].read = read;
	device_table[index].write = write;
}

/* 初始化device_table */
void device_init()
{
	memset(device_table, 0, sizeof(device_table));

    /*
     * name 使用固定长度数组，避免 device_register()
     * 固定复制 MAXLEN_FILENAME 字节时越过字符串字面量。
     */
    char stdin_name[MAXLEN_FILENAME] = "stdin";
    char stdout_name[MAXLEN_FILENAME] = "stdout";
    char stderr_name[MAXLEN_FILENAME] = "stderr";
    char zero_name[MAXLEN_FILENAME] = "zero";
    char null_name[MAXLEN_FILENAME] = "null";
    char gpt0_name[MAXLEN_FILENAME] = "gpt0";

    device_register(
        INODE_MAJOR_STDIN,
        stdin_name,
        device_stdin_read,
        NULL
    );

    device_register(
        INODE_MAJOR_STDOUT,
        stdout_name,
        NULL,
        device_stdout_write
    );

    device_register(
        INODE_MAJOR_STDERR,
        stderr_name,
        NULL,
        device_stderr_write
    );

    device_register(
        INODE_MAJOR_ZERO,
        zero_name,
        device_zero_read,
        NULL
    );

    device_register(
        INODE_MAJOR_NULL,
        null_name,
        device_null_read,
        device_null_write
    );

    device_register(
        INODE_MAJOR_GPT0,
        gpt0_name,
        NULL,
        device_gpt0_write
    );

    /*
     * /dev 是所有设备文件的父目录。
     */
    inode_t *ip = path_to_inode("/dev");

    if (ip == NULL) {
        ip = path_create_inode(
            "/dev",
            INODE_TYPE_DIR,
            INODE_MAJOR_DEFAULT,
            INODE_MINOR_DEFAULT
        );
    }

    assert(ip != NULL,
        "device_init: create /dev failed");

    inode_lock(ip);
    assert(ip->disk_info.type == INODE_TYPE_DIR,
        "device_init: /dev is not directory");
    inode_unlock(ip);
    inode_put(ip);

    /*
     * 确保六个设备文件存在，并检查其设备号。
     */
    char paths[6][MAXLEN_FILENAME] = {
        "/dev/stdin",
        "/dev/stdout",
        "/dev/stderr",
        "/dev/zero",
        "/dev/null",
        "/dev/gpt0"
    };

    uint16 majors[6] = {
        INODE_MAJOR_STDIN,
        INODE_MAJOR_STDOUT,
        INODE_MAJOR_STDERR,
        INODE_MAJOR_ZERO,
        INODE_MAJOR_NULL,
        INODE_MAJOR_GPT0
    };

    for (uint32 i = 0; i < 6; i++) {
        ip = path_to_inode(paths[i]);

        if (ip == NULL) {
            ip = path_create_inode(
                paths[i],
                INODE_TYPE_DIVICE,
                majors[i],
                INODE_MINOR_DEFAULT
            );
        }

        assert(ip != NULL,
            "device_init: create device failed");

        inode_lock(ip);

        assert(
            ip->disk_info.type == INODE_TYPE_DIVICE &&
            ip->disk_info.major == majors[i] &&
            ip->disk_info.minor == INODE_MINOR_DEFAULT,
            "device_init: invalid device inode"
        );

        inode_unlock(ip);
        inode_put(ip);
    }
}

/* 检查文件major字段的合法性 */
bool device_open_check(uint16 major, uint32 open_mode)
{
    /* 防止设备号越界 */
    if (major >= N_DEVICE)
        return false;

    device_t *device = &device_table[major];

    /* name为空说明该主设备号尚未注册 */
    if (device->name[0] == '\0')
        return false;

    /* 请求读权限，但设备没有提供读函数 */
    if ((open_mode & FILE_OPEN_READ) &&
        device->read == NULL)
        return false;

    /* 请求写权限，但设备没有提供写函数 */
    if ((open_mode & FILE_OPEN_WRITE) &&
        device->write == NULL)
        return false;

    return true;
}

/* 从设备文件中读取数据 */
uint32 device_read_data(uint16 major, uint32 len, uint64 dst, bool is_user_dst)
{
    if (major >= N_DEVICE)
        return 0;

    device_t *device = &device_table[major];

    if (device->name[0] == '\0' ||
        device->read == NULL)
        return 0;

    return device->read(
        len,
        dst,
        is_user_dst
    );
}

/* 向设备文件写入数据 */
uint32 device_write_data(uint16 major, uint32 len, uint64 src, bool is_user_src)
{
    if (major >= N_DEVICE)
        return 0;

    device_t *device = &device_table[major];

    if (device->name[0] == '\0' ||
        device->write == NULL)
        return 0;

    return device->write(
        len,
        src,
        is_user_src
    );
}
