#include "mod.h"

// 内核空间和用户空间的可分配物理页分开描述
static alloc_region_t kern_region, user_region;

static void alloc_region_init(alloc_region_t *ar, char *name, uint64 begin, uint64 end){
    ar->begin = begin;
    ar->end = end;
    spinlock_init(&ar->lk, name);
    ar->allocable = 0;
    ar->list_head.next = NULL;

    /* 反向遍历并头插，使后续分配按物理地址递增。 */
    for(uint64 p = end; p > begin;){
        p -= PGSIZE;
        page_node_t *node = (page_node_t *)p;
        node->next = ar->list_head.next;
        ar->list_head.next = node;
        ar->allocable++;
    }
}
// 物理内存的初始化
// 本质上就是填写kern_region和user_region, 包括基本数值和空闲链表
void pmem_init(void)
{
    uint64 kern_begin = (uint64)ALLOC_BEGIN;
    uint64 kern_end = kern_begin + KERN_PAGES * PGSIZE;
    uint64 user_begin = kern_end;
    uint64 user_end = (uint64)ALLOC_END;

    assert(kern_end <= user_end, "pmem_init: no enough memory");

    alloc_region_init(&kern_region, "kern_region", kern_begin, kern_end);
    alloc_region_init(&user_region, "user_region", user_begin, user_end);
}

// 尝试返回一个可分配的清零后的物理页
// 失败则panic锁死
void* pmem_alloc(bool in_kernel)
{
    page_node_t *page;
    alloc_region_t *ar = in_kernel ? &kern_region : &user_region;
    spinlock_acquire(&ar->lk);
    page = ar->list_head.next;
    if(page == NULL){
        panic("pmem_alloc: out of memory");
    }
    ar->list_head.next = page->next;
    ar->allocable--;

    spinlock_release(&ar->lk);

    memset(page, 0, PGSIZE);
    return page;
}

// 释放一个物理页
// 失败则panic锁死
void pmem_free(uint64 page, bool in_kernel)
{
    alloc_region_t *ar = in_kernel ? &kern_region : &user_region;

    assert(page % PGSIZE == 0, "pmem_free: page not aligned");
    assert(page >= ar->begin && page < ar->end, "pmem_free: page out of range");

    memset((void *)page, 0, PGSIZE);

    spinlock_acquire(&ar->lk);
    
    page_node_t *node = (page_node_t *)page;
    node->next = ar->list_head.next;
    ar->list_head.next = node;
    ar->allocable++;

    spinlock_release(&ar->lk);
}
