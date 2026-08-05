#include "mod.h"
#include "../fs/type.h"
#include "../fs/method.h"

/*
	将ELF文件中的segment放入内存中制定位置
	inode逻辑区域: [seg_start, seg_start + len)
	进程地址空间: [va_start, va_start + len), 对应的物理页是存在的
*/
static void load_segment(inode_t *ip, pgtbl_t pgtbl, 
	uint64 seg_start, uint64 va_start, uint32 len)
{
	assert(va_start % PGSIZE == 0, "load_segment: va aligned!");

	pte_t *pte;
	uint64 pa;
	uint32 read_len, cut_len;

	for (read_len = 0; read_len < len; read_len += PGSIZE)
	{
		/* 获取物理内存地址 */
		pte = vm_getpte(pgtbl, va_start + read_len, false);
		pa = PTE_TO_PA(*pte);
		assert(pa != 0, "load_segment: invalid pa!");

		/* 读入segment的一部分 */
		cut_len = MIN(len - read_len, PGSIZE);
		if (inode_read_data(ip, (uint32)seg_start + read_len, cut_len, (void*)pa, false) != cut_len)
			panic("load_segment: read fail!");
	}
}

/* 将程序的代码区和数据区读入用户堆中, 返回new_heap_top */
static uint64 prepare_heap(pgtbl_t new_pgtbl, inode_t *ip, elf_header_t *eh)
{
	program_header_t ph;
	uint64 new_heap_top = USER_BASE, old_heap_top = USER_BASE;
	
	for (uint32 off = eh->ph_off; off < eh->ph_off + eh->ph_ent_num * sizeof(ph); off += sizeof(ph))
	{
		// 读入一个program header
		if (inode_read_data(ip, off, sizeof(ph), &ph, false) != sizeof(ph))
			return -1;
		
		// 判断是否有必要载入
		if (ph.type != ELF_PROG_LOAD)
			continue;
		
		// program header参数的合法性检查
		if (ph.mem_size < ph.file_size)
			return -1;
		if (ph.va + ph.mem_size < ph.va)
			return -1;
		if (ph.va % PGSIZE != 0)
			return -1;
		
		int pte_flags = 0;

		if (ph.flags & ELF_PROG_FLAG_READ)
			pte_flags |= PTE_R;

		if (ph.flags & ELF_PROG_FLAG_WRITE)
			pte_flags |= PTE_R | PTE_W;

		if (ph.flags & ELF_PROG_FLAG_EXEC)
			pte_flags |= PTE_X;

		if (pte_flags == 0)
			return -1;

		// 用户堆生长
		new_heap_top = uvm_heap_grow(
						new_pgtbl,
						old_heap_top,
						ph.va + ph.mem_size - old_heap_top,
						pte_flags
					);
		if (new_heap_top != ph.va + ph.mem_size)
			return -1;
		old_heap_top = new_heap_top;

		// segment读入
		load_segment(ip, new_pgtbl, ph.off, ph.va, ph.file_size);
	}

	return new_heap_top;
}

/* 准备栈空间用于存储输入参数(4KB), 设置arg_count, 返回sp */
static uint64 prepare_stack(pgtbl_t new_pgtbl, char **argv, int *arg_count)
{
	uint64 ustack_page;
	uint64 sp = TRAPFRAME, sp_base = TRAPFRAME - PGSIZE;
	uint64 sp_list[ELF_MAXARGS + 1];
	uint32 argc, arg_len;

	ustack_page = (uint64)pmem_alloc(false);
	vm_mappages(new_pgtbl, sp_base, ustack_page, PGSIZE, PTE_R | PTE_W | PTE_U);
	
	for (argc = 0; argv[argc] != NULL; argc++)
	{
		if (argc >= ELF_MAXARGS)
			return -1;
		
		arg_len = strlen(argv[argc]) + 1;
		sp -= ALIGN_UP(arg_len, 16);
		if (sp < sp_base)
			return -1;
		
		uvm_copyout(new_pgtbl, sp, (uint64)argv[argc], arg_len);

		sp_list[argc] = sp;
	}
	sp_list[argc] = 0;

	arg_len = (argc + 1) * sizeof(uint64);
	sp -= ALIGN_UP(arg_len, 16);
	if (sp < sp_base)
		return -1;

	uvm_copyout(new_pgtbl, sp, (uint64)sp_list, arg_len);

	*arg_count = argc;

	return sp;
}

/*
	执行ELF文件
	输入路径和参数
	成功返回argc, 失败返回-1
*/
int proc_exec(char *path, char **argv)
{
	if (path == NULL || path[0] == '\0' ||
        argv == NULL)
        return -1;

    proc_t *p = myproc();
    if (p == NULL)
        return -1;

    /*
     * 先构造全新的地址空间。失败时旧进程仍可继续运行。
     */
    trapframe_t *new_tf =
        (trapframe_t *)pmem_alloc(true);

    pgtbl_t new_pgtbl =
        proc_pgtbl_init((uint64)new_tf);

    inode_t *ip = path_to_inode(path);
    if (ip == NULL)
        goto fail;

    elf_header_t eh;
    inode_lock(ip);

    /*
     * 检查文件类型和ELF头。
     */
    if (ip->disk_info.type != INODE_TYPE_DATA ||
        inode_read_data(
            ip, 0, sizeof(eh), &eh, false
        ) != sizeof(eh) ||
        eh.magic != ELF_MAGIC ||
        eh.eh_size != sizeof(elf_header_t) ||
        eh.ph_ent_size != sizeof(program_header_t)) {
        goto fail_inode;
    }

    /*
     * program header table必须完整位于文件内。
     */
    uint64 ph_table_size =
        (uint64)eh.ph_ent_num *
        sizeof(program_header_t);

    if (eh.ph_off > ip->disk_info.size ||
        ph_table_size >
            ip->disk_info.size - eh.ph_off) {
        goto fail_inode;
    }

    uint64 new_heap_top =
        prepare_heap(new_pgtbl, ip, &eh);

    if (new_heap_top == (uint64)-1 ||
        eh.entry < USER_BASE ||
        eh.entry >= new_heap_top) {
        goto fail_inode;
    }

    inode_unlock(ip);
    inode_put(ip);
    ip = NULL;

    int argc = 0;
    uint64 new_sp =
        prepare_stack(new_pgtbl, argv, &argc);

    if (new_sp == (uint64)-1)
        goto fail;

    /*
     * 新地址空间已经完整构造，开始提交。
     */
    pgtbl_t old_pgtbl = p->pgtbl;
    mmap_region_t *old_mmap = p->mmap;

    new_tf->a0 = argc;
    new_tf->a1 = new_sp;
    new_tf->user_to_kern_epc = eh.entry;
    new_tf->sp = new_sp;

    p->pgtbl = new_pgtbl;
    p->tf = new_tf;
    p->heap_top = new_heap_top;
    p->ustack_npage = 1;
    p->mmap = NULL;

    /*
     * 使用路径最后一个分量作为进程名。
     */
    char *base = path;

    for (char *s = path; *s != '\0'; s++) {
        if (*s == '/' && s[1] != '\0')
            base = s + 1;
    }

    uint32 name_len = strlen(base);
    name_len = MIN(name_len, PROC_NAME_LEN - 1);

    memset(p->name, 0, sizeof(p->name));
    memmove(p->name, base, name_len);

    /*
     * 新地址空间提交后，才能销毁旧地址空间。
     */
    uvm_destroy_pgtbl(old_pgtbl);

    while (old_mmap != NULL) {
        mmap_region_t *next = old_mmap->next;
        mmap_region_free(old_mmap);
        old_mmap = next;
    }

    return argc;

fail_inode:
    inode_unlock(ip);
    inode_put(ip);

fail:
    uvm_destroy_pgtbl(new_pgtbl);
    return -1;
}
