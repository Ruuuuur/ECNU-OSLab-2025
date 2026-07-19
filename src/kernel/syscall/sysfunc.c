#include "mod.h"

/*
    测试: 从用户空间传入一个int类型的数组
    uint64 addr 数组起始地址
    uint32 len  元素数量
    成功返回0
*/
uint64 sys_copyin()
{
    proc_t *p = myproc();

    uint64 addr;
    uint32 len;
    arg_uint64(0, &addr);
    arg_uint32(1, &len);

    int values[5];
    assert(len <= 5, "sys_copyin: len too large");

    uvm_copyin(
        p->pgtbl,
        (uint64)values,
        addr,
        len * sizeof(int)
    );

    printf("copyin: ");
    for (uint32 i = 0; i < len; i++)
        printf("%d ", values[i]);
    printf("\n");

    return 0;
}

/*
    测试: 向用户空间传出一个int类型的数组
    uint64 addr 数组起始地址
    成功返回拷贝的元素数量
*/
uint64 sys_copyout()
{
    proc_t *p = myproc();

    uint64 addr;
    arg_uint64(0, &addr);

    int values[5] = {1, 2, 3, 4, 5};

    uvm_copyout(
        p->pgtbl,
        addr,
        (uint64)values,
        sizeof(values)
    );
	return 5;
}

/*
    测试: 从用户空间传入一个字符串
    uint64 addr 字符串起始地址
    成功返回0
*/
uint64 sys_copyinstr()
{
    char str[STR_MAXLEN];

    arg_str(0, str, STR_MAXLEN);
    printf("copyinstr: %s\n", str);

    return 0;
}

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

        // uvm_heap_grow() 的 len 参数是 uint32
        if(len > 0xfffffffful){
            return -1;
        }

        result = uvm_heap_grow(p->pgtbl, old_heap_top, (uint32)len);
    }else{
        len = old_heap_top - new_heap_top;
        // uvm_heap_ungrow() 的 len 参数是 uint32
        if(len > 0xfffffffful){
            return -1;
        }

        result = uvm_heap_ungrow(p->pgtbl, old_heap_top, (uint32)len);
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

    if(len == 0 || len % PGSIZE != 0){
        return -1;
    }

    if(begin != 0){
        if(begin % PGSIZE != 0){
            return -1;
        }

        if(begin < MMAP_BEGIN || begin >= MMAP_END){
            return -1;
        }

        if((uint64)len > MMAP_END - begin){
            return -1;
        }
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

    if(len == 0 || len % PGSIZE != 0){
        return -1;
    }

    if (begin % PGSIZE != 0)
        return -1;

    if (begin < MMAP_BEGIN || begin >= MMAP_END)
        return -1;

    if ((uint64)len > MMAP_END - begin)
        return -1;

    uvm_munmap(begin, len / PGSIZE);

    return 0;
}
