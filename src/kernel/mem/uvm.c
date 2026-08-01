#include "mod.h"

/*--------------------part-1: 关于内核空间<->用户空间的数据传递--------------------*/

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    while(len > 0){
        uint64 va_page = ALIGN_DOWN(src, PGSIZE);
        uint64 offset = src - va_page;

        pte_t *pte = vm_getpte(pgtbl, va_page, false);
        assert(pte != NULL, "uvm_copyin: pte not found");
        assert((*pte & PTE_V) != 0, "uvm_copyin: invalid pte");
        assert(!PTE_CHECK(*pte), "uvm_copyin: not leaf");
        assert((*pte & PTE_U) != 0, "uvm_copyin: not user page");
        assert((*pte & PTE_R) != 0, "uvm_copyin: not readable");

        uint32 n = MIN(len, PGSIZE - offset);
        uint64 pa = PTE_TO_PA(*pte) + offset;

        memmove((void *)dst, (void *)pa, n);

        src += n;
        dst += n;
        len -= n;
    }
}

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    while(len > 0){
        uint64 va_page = ALIGN_DOWN(dst, PGSIZE);
        uint64 offset = dst - va_page;

        pte_t *pte = vm_getpte(pgtbl, va_page, false);
        assert(pte != NULL, "uvm_copyout: pte not found");
        assert((*pte & PTE_V) != 0, "uvm_copyout: invalid pte");
        assert(!PTE_CHECK(*pte), "uvm_copyout: not leaf");
        assert((*pte & PTE_U) != 0, "uvm_copyout: not user page");
        assert((*pte & PTE_W) != 0, "uvm_copyout: not writable");

        uint32 n = MIN(len, PGSIZE - offset);
        uint64 pa = PTE_TO_PA(*pte) + offset;

        memmove((void *)pa, (void *)src, n);

        dst += n;
        src += n;
        len -= n;
    }
}

// 用户态字符串拷贝到内核态
// 最多拷贝maxlen字节, 中途遇到'\0'则终止
// 注意: src dst 不一定是 page-aligned
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    while (maxlen > 0) {
        // 查询 src 当前所在的用户页
        uint64 va_page = ALIGN_DOWN(src, PGSIZE);
        uint64 offset = src - va_page;

        pte_t *pte = vm_getpte(pgtbl, va_page, false);

        assert(pte != NULL, "uvm_copyin_str: pte not found");
        assert((*pte & PTE_V) != 0, "uvm_copyin_str: invalid pte");
        assert(!PTE_CHECK(*pte), "uvm_copyin_str: not leaf");
        assert((*pte & PTE_U) != 0, "uvm_copyin_str: not user page");
        assert((*pte & PTE_R) != 0, "uvm_copyin_str: not readable");

        uint32 n = MIN(maxlen, PGSIZE - offset);
        char *s = (char *)(PTE_TO_PA(*pte) + offset);

        // 逐字节复制当前页中的字符
        while (n > 0) {
            char c = *s;
            *(char *)dst = c;

            if (c == '\0')
                return;

            s++;
            src++;
            dst++;
            n--;
            maxlen--;
        }
    }

    assert(false, "uvm_copyin_str: string too long");
}

/*--------------------part-2: mmap_region相关--------------------*/

// 打印以mmap为首的mmap链
// for debug
void uvm_show_mmaplist(mmap_region_t *mmap)
{
    mmap_region_t *tmp = mmap;
    printf("\nalloced mmap_space:\n");
    if (tmp == NULL)
        printf("empty\n");
    while (tmp != NULL)
    {
        printf("alloced mmap_region: %p ~ %p\n", tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
    }
}

// 两个 mmap_region 区域合并
// 注意: 保留一个 释放一个 不操作 next 指针
// 由uvm_mmap调用
static void mmap_merge(mmap_region_t *mmap_1, mmap_region_t *mmap_2, bool keep_mmap_1)
{
    // 确保有效和紧临
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin, "mmap_merge: check fail");

    // merge
    if (keep_mmap_1) {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    } else {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}

// 寻找一块足够大的区域(len), 作为 mmap_region
// 由uvm_mmap调用(处理begin==0的情况)
// 成功返回begin, 失败返回0
static uint64 uvm_mmap_find(mmap_region_t *head_mmap, uint64 len, mmap_region_t **p_last_mmap, mmap_region_t **p_tmp_mmap)
{
    if(len == 0 || len > MMAP_END - MMAP_BEGIN){
        return 0;
    }

    uint64 candidate = MMAP_BEGIN;
    mmap_region_t *last = NULL;
    mmap_region_t *tmp = head_mmap;

    while(tmp != NULL){
        //链表必须按照begin递增
        assert(candidate <= tmp->begin,
            "uvm_mmap_find: mmap list unordered");

        //[candidate, tem->begin)是当前空隙
        if(len <= tmp->begin - candidate){
            *p_last_mmap = last;
            *p_tmp_mmap = tmp;
            return candidate;
        }
        candidate = tmp->begin + tmp->npages * PGSIZE;
        last = tmp;
        tmp = tmp->next;
    }

    //tmp == NULL
    if(candidate <= MMAP_END &&
        len <= MMAP_END - candidate){
            *p_last_mmap = last;
            *p_tmp_mmap = NULL;
            return candidate;
        }

    return 0;
}

// 在用户页表和进程mmap链里新增mmap区域 [begin, begin + npages * PGSIZE)
// 调用者保证begin是page-aligned的, 页面权限为perm
// 注意: 如果start==0, 意味着需要内核自主找一块足够大的空间
// 失败则panic卡死
uint64 uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    proc_t *p = myproc();

    assert(npages > 0, "uvm_mmap: npages is zero");

    uint64 len = (uint64)npages * PGSIZE;
    mmap_region_t *last = NULL;
    mmap_region_t *tmp = p->mmap;

    if(begin == 0){
        begin = uvm_mmap_find(p->mmap, len, &last, &tmp);
        assert(begin != 0, "uvm_mmap: no enough space");
    }else{
        assert(begin % PGSIZE == 0,
            "uvm_mmap: begin not aligned");
        assert(begin >= MMAP_BEGIN && begin < MMAP_END,
            "uvm_mmap: begin out of range");
        assert(len <= MMAP_END - begin,
            "uvm_mmap: end out of range");

        //找到第一个begin不小于新区域的节点
        while(tmp != NULL && tmp->begin < begin){
            last = tmp;
            tmp = tmp->next;
        }

        //检查是否与前一个节点重叠
        if(last != NULL){
            uint64 last_end = last->begin + last->npages * PGSIZE;
            assert(last_end <= begin,
                "uvm_mmap: overlap previous");
        }

        // 检查是否与后一个节点重叠
        if (tmp != NULL) {
            assert(begin + len <= tmp->begin,
                "uvm_mmap: overlap next");
        }
    }
    mmap_region_t *new_mmap = mmap_region_alloc();

    new_mmap->begin = begin;
    new_mmap->npages = npages;
    new_mmap->next = tmp;

    if(last == NULL){
        p->mmap = new_mmap;
    }else{
        last->next = new_mmap;
    }

    /*
     * 如果和前一个区域相邻，保留 last，释放 new_mmap。
     * mmap_merge() 不修改 next，所以需要先绕过即将释放的节点。
     */
    if (last != NULL &&
        last->begin + last->npages * PGSIZE == new_mmap->begin) {
        last->next = new_mmap->next;
        mmap_merge(last, new_mmap, true);
        new_mmap = last;
    }

    /*
     * new_mmap 现在是合并后的有效节点。
     * 再判断它是否和后一个节点相邻。
     */
    tmp = new_mmap->next;

    if (tmp != NULL &&
        new_mmap->begin + new_mmap->npages * PGSIZE == tmp->begin) {
        new_mmap->next = tmp->next;
        mmap_merge(new_mmap, tmp, true);
    }

    // 为新增加的虚拟区域逐页申请物理页
    for (uint32 i = 0; i < npages; i++) {
        uint64 va = begin + (uint64)i * PGSIZE;
        void *page = pmem_alloc(false);

        vm_mappages(
            p->pgtbl,
            va,
            (uint64)page,
            PGSIZE,
            perm | PTE_U
        );
    }
    return begin;
}


// 在用户页表和进程mmap链里释放mmap区域 [begin, begin + npages * PGSIZE)
// 失败则panic卡死
void uvm_munmap(uint64 begin, uint32 npages)
{
    proc_t *p = myproc();

    assert(npages > 0, "uvm_munmap: npages is zero");
    assert(begin % PGSIZE == 0,
        "uvm_munmap: begin not aligned");
    assert(begin >= MMAP_BEGIN && begin < MMAP_END,
        "uvm_munmap: begin out of range");
    uint64 len = (uint64)npages * PGSIZE;

    assert(len <= MMAP_END - begin,
        "uvm_munmap: end out of range");

    uint64 end = begin + len;

    mmap_region_t *last = NULL;
    mmap_region_t *tmp = p->mmap;

    // 找到第一个结束地址大于 begin 的节点
    while (tmp != NULL) {
        uint64 tmp_end =
            tmp->begin + tmp->npages * PGSIZE;

        if (begin < tmp_end)
            break;

        last = tmp;
        tmp = tmp->next;
    }

    assert(tmp != NULL, "uvm_munmap: region not found");

    uint64 tmp_begin = tmp->begin;
    uint64 tmp_end = tmp->begin + tmp->npages * PGSIZE;

    // 要释放的区间必须完整包含在这个已分配节点中
    assert(begin >= tmp_begin && end <= tmp_end,
        "uvm_munmap: region not allocated");

    if(begin == tmp_begin && end == tmp_end){
        // 情况 1：整个节点全部释放
        if (last == NULL)
            p->mmap = tmp->next;
        else
            last->next = tmp->next;

        mmap_region_free(tmp);
    }else if(begin == tmp_begin) {
        // 情况 2：释放节点前部
        tmp->begin = end;
        tmp->npages = (tmp_end - end) / PGSIZE;

    }else if(end == tmp_end) {
        // 情况 3：释放节点后部
        tmp->npages = (begin - tmp_begin) / PGSIZE;

    }else{
        // 情况 4：释放节点中间，需要拆成左右两个节点
        mmap_region_t *right = mmap_region_alloc();

        right->begin = end;
        right->npages = (tmp_end - end) / PGSIZE;
        right->next = tmp->next;

        tmp->npages = (begin - tmp_begin) / PGSIZE;
        tmp->next = right;
    }

    // 清除用户页表映射并归还对应物理页
    vm_unmappages(
        p->pgtbl,
        begin,
        len,
        true
    );
}

/*------------------part-3: 用户空间heap和stack管理相关------------------*/

// 用户堆空间增加, 返回新的堆顶地址 (注意栈顶最大值限制)
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len, int flag) 
{
    uint64 new_heap_top = cur_heap_top + len;

    // 检查整数溢出和 mmap 区域边界
    if (new_heap_top < cur_heap_top || new_heap_top > MMAP_BEGIN)
        return -1;

    uint64 begin = ALIGN_UP(cur_heap_top, PGSIZE);
    uint64 end = ALIGN_UP(new_heap_top, PGSIZE);

    for(uint64 va = begin; va < end; va += PGSIZE){
        void *page = pmem_alloc(false);

        vm_mappages(
            pgtbl,
            va,
            (uint64)page,
            PGSIZE,
            PTE_R | PTE_W | PTE_U
        );
    }
    return new_heap_top;
}

// 用户堆空间减少, 返回新的堆顶地址
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len)
{
    uint64 heap_begin = USER_BASE + PGSIZE;
    if(cur_heap_top < heap_begin || len > cur_heap_top - heap_begin) return -1;

    uint64 new_heap_top = cur_heap_top - len;
    uint64 begin = ALIGN_UP(new_heap_top, PGSIZE);
    uint64 end = ALIGN_UP(cur_heap_top, PGSIZE);

    if(begin < end) vm_unmappages(pgtbl, begin, end - begin, true);

    return new_heap_top;
}

// 处理函数栈增长导致的page fault事件
// 成功返回new_ustack_npage，失败返回-1
uint64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_ustack_npage, uint64 fault_addr)
{
    uint64 max_npage = (TRAPFRAME - MMAP_END) / PGSIZE;

    if(old_ustack_npage == 0 || old_ustack_npage > max_npage){
        return -1;
    }

    uint64 old_bottom = TRAPFRAME - old_ustack_npage * PGSIZE;

    if(fault_addr < MMAP_END || fault_addr >= old_bottom){
        return -1;
    }

    uint64 new_bottom = ALIGN_DOWN(fault_addr, PGSIZE);

    for(uint64 va = new_bottom; va < old_bottom; va += PGSIZE){
        void *page = pmem_alloc(false);

        vm_mappages(
            pgtbl,
            va,
            (uint64)page,
            PGSIZE,
            PTE_R | PTE_W | PTE_U
        );
    }
    return (TRAPFRAME - new_bottom) / PGSIZE;
}

/*----------------------part-4: 用户页表管理相关----------------------*/

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表level = 3
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    assert(pgtbl != NULL, "destroy_pgtbl: NULL");
    assert(level >= 1 && level <= 3,
        "destroy_pgtbl: invalid level");

    for(uint32 i = 0; i < PGSIZE / sizeof(pte_t); ++i){
        pte_t pte = pgtbl[i];

        if((pte & PTE_V) == 0){
            continue;
        }

        uint64 pa = PTE_TO_PA(pte);

        if (PTE_CHECK(pte)) {
            // R/W/X 全为零，指向下一级页表
            assert(level > 1,
                "destroy_pgtbl: child below level 1");

            destroy_pgtbl(
                (pgtbl_t)pa,
                level - 1
            );
        } else {
            // 当前内核只使用 4KB 叶子页
            assert(level == 1,
                "destroy_pgtbl: huge page unsupported");

            pmem_free(pa, false);
        }

        pgtbl[i] = 0;
    }

    // 当前页表页由 pmem_alloc(true) 分配
    pmem_free((uint64)pgtbl, true);
}

// 页表销毁
void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    pte_t *tf_pte = vm_getpte(pgtbl, TRAPFRAME, false);

    assert(tf_pte != NULL && (*tf_pte & PTE_V),
        "uvm_destroy_pgtbl: trapframe missing");

    uint64 tf_pa = PTE_TO_PA(*tf_pte);

    // 只清除映射，再按内核物理页释放 trapframe
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, false);
    pmem_free(tf_pa, true);

    // trampoline 为所有进程共享，不能释放其物理页
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false);

    // 释放剩余用户数据页和三级页表本身
    destroy_pgtbl(pgtbl, 3);
}

// 连续虚拟空间的复制
// 在uvm_copy_pgtbl中使用
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, page;
    int flags;
    pte_t *pte;

    for (va = begin; va < end; va += PGSIZE)
    {
        pte = vm_getpte(old, va, false);
        assert(pte != NULL, "uvm_copy_pgtbl: pte == NULL");
        assert((*pte) & PTE_V, "uvm_copy_pgtbl: pte not valid");
        assert(!PTE_CHECK(*pte),
            "uvm_copy_pgtbl: pte not leaf");
        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        memmove((char *)page, (const char *)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 拷贝页表 (拷贝并不包括 trapframe 和 trampoline)
// 拷贝的页表管理的物理页是原来页表的复制品
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint64 ustack_npage, mmap_region_t *mmap)
{
    assert(old != NULL, "uvm_copy_pgtbl: old is NULL");
    assert(new != NULL, "uvm_copy_pgtbl: new is NULL");

    /*
     * 1. 复制代码和数据页。
     * 当前 initcode 固定占用 USER_BASE 开始的一页。
     */
    copy_range(
        old,
        new,
        USER_BASE,
        USER_BASE + PGSIZE
    );

    /*
     * 2. 复制堆。
     * heap_top 是字节边界，实际映射范围需要向上对齐。
     */
    uint64 heap_begin = USER_BASE + PGSIZE;

    assert(heap_top >= heap_begin && heap_top <= MMAP_BEGIN,
        "uvm_copy_pgtbl: invalid heap top");

    uint64 heap_end = ALIGN_UP(heap_top, PGSIZE);

    if (heap_begin < heap_end)
        copy_range(old, new, heap_begin, heap_end);

    /*
     * 3. 复制所有 mmap 区域。
     */
    mmap_region_t *tmp = mmap;

    while (tmp != NULL) {
        assert(tmp->npages > 0,
            "uvm_copy_pgtbl: empty mmap");
        assert(tmp->begin % PGSIZE == 0,
            "uvm_copy_pgtbl: mmap not aligned");
        assert(tmp->begin >= MMAP_BEGIN &&
            tmp->begin < MMAP_END,
            "uvm_copy_pgtbl: mmap out of range");

        uint64 mmap_end =
            tmp->begin + (uint64)tmp->npages * PGSIZE;

        assert(mmap_end <= MMAP_END,
            "uvm_copy_pgtbl: mmap end out of range");

        copy_range(
            old,
            new,
            tmp->begin,
            mmap_end
        );

        tmp = tmp->next;
    }

    /*
     * 4. 复制用户栈。
     */
    uint64 max_stack_npage =
        (TRAPFRAME - MMAP_END) / PGSIZE;

    assert(ustack_npage > 0 &&
        ustack_npage <= max_stack_npage,
        "uvm_copy_pgtbl: invalid stack size");

    uint64 stack_begin =
        TRAPFRAME - ustack_npage * PGSIZE;

    copy_range(
        old,
        new,
        stack_begin,
        TRAPFRAME
    );
}
