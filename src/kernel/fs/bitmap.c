#include "mod.h"

extern super_block_t sb;

/*
	查询一个block中的所有bit, 找到空闲bit, 设置1并返回
	如果没有空闲bit, 返回-1
*/
static uint32 bitmap_search_and_set(uint32 bitmap_block_num, uint32 valid_count)
{
    assert(valid_count <= BIT_PER_BLOCK,
        "bitmap_search_and_set: invalid count");

    buffer_t *buf = buffer_get(bitmap_block_num);

    for(uint32 index = 0; index < valid_count; index++){
        uint32 byte = index / BIT_PER_BYTE;
        uint32 shift = index % BIT_PER_BYTE;
        uint8 mask = (uint8)(1U << shift);

        if((buf->data[byte] & mask) == 0){
            buf->data[byte] |= mask;

            buffer_write(buf);
            buffer_put(buf);
            return index;
        }
    }

    buffer_put(buf);
    return (uint32)-1;
}

/*
	将block中第index个bit设为0
*/
static void bitmap_clear(uint32 bitmap_block_num, uint32 index)
{
    assert(index < BIT_PER_BLOCK,
        "bitmap_clear: invalid index");

    buffer_t *buf = buffer_get(bitmap_block_num);

    uint32 byte = index / BIT_PER_BYTE;
    uint32 shift = index % BIT_PER_BYTE;
    uint8 mask = (uint8)(1U << shift);

    assert(buf->data[byte] & mask,
        "bitmap_clear: bit already clear");

    buf->data[byte] &= (uint8)~mask;

    buffer_write(buf);
    buffer_put(buf);
}

/*
	获取一个空闲block, 将data_bitmap对应bit设为1
	返回这个block的全局序号
*/
uint32 bitmap_alloc_block()
{
    for(uint32 block = 0;
        block < sb.data_bitmap_blocks;
        block++){

        uint32 base = block * BIT_PER_BLOCK;
        uint32 valid_count = sb.data_blocks - base;

        if(valid_count > BIT_PER_BLOCK)
            valid_count = BIT_PER_BLOCK;

        uint32 local_index = bitmap_search_and_set(
            sb.data_bitmap_firstblock + block,
            valid_count
        );

        if(local_index != (uint32)-1){
            return sb.data_firstblock + base + local_index;
        }
    }

    return (uint32)-1;
}

/*
	获取一个空闲inode, 将inode_bitmap对应bit设为1
	返回这个inode的全局序号
*/
uint32 bitmap_alloc_inode()
{
    for(uint32 block = 0;
        block < sb.inode_bitmap_blocks;
        block++){

        uint32 base = block * BIT_PER_BLOCK;
        uint32 valid_count = sb.total_inodes - base;

        if(valid_count > BIT_PER_BLOCK)
            valid_count = BIT_PER_BLOCK;

        uint32 local_index = bitmap_search_and_set(
            sb.inode_bitmap_firstblock + block,
            valid_count
        );

        if(local_index != (uint32)-1){
            return base + local_index;
        }
    }

    return (uint32)-1;
}

/* 释放一个block, 将data_bitmap对应bit设为0 */
void bitmap_free_block(uint32 block_num)
{
    assert(block_num >= sb.data_firstblock &&
            block_num < sb.data_firstblock + sb.data_blocks,
        "bitmap_free_block: invalid block number");

    uint32 relative = block_num - sb.data_firstblock;

    uint32 bitmap_block_num =
        sb.data_bitmap_firstblock +
        relative / BIT_PER_BLOCK;

    uint32 local_index = relative % BIT_PER_BLOCK;

    bitmap_clear(bitmap_block_num, local_index);
}

/* 释放一个inode, 将inode_bitmap对应bit设为0 */
void bitmap_free_inode(uint32 inode_num)
{
    assert(inode_num < sb.total_inodes,
        "bitmap_free_inode: invalid inode number");

    uint32 bitmap_block_num =
        sb.inode_bitmap_firstblock +
        inode_num / BIT_PER_BLOCK;

    uint32 local_index = inode_num % BIT_PER_BLOCK;

    bitmap_clear(bitmap_block_num, local_index);
}

/* 打印某个bitmap中所有分配出去的bit */
void bitmap_print(bool print_data_bitmap)
{
    uint32 first_block, bitmap_blocks, total_bits;
    uint32 global_base, current_bit = 0;

    if (print_data_bitmap) {
		printf("data bitmap alloced bits:\n");
        first_block = sb.data_bitmap_firstblock;
        bitmap_blocks = sb.data_bitmap_blocks;
        total_bits = sb.data_blocks;
        global_base = sb.data_firstblock;
    } else {
		printf("inode bitmap alloced bits:\n");
		first_block = sb.inode_bitmap_firstblock;
        bitmap_blocks = sb.inode_bitmap_blocks;
        total_bits = sb.total_inodes;
        global_base = 0;
    }

    for (uint32 block = 0; block < bitmap_blocks; block++)
	{
        uint32 bitmap_block_num = first_block + block;
        uint32 bits_in_this_block = BIT_PER_BLOCK;

        // 最后一个 block 可能不满
        if (current_bit + BIT_PER_BLOCK > total_bits)
            bits_in_this_block = total_bits - current_bit;

        buffer_t *buf = buffer_get(bitmap_block_num);

        // 遍历该 block 中的有效 bit
        for (uint32 byte = 0; byte < bits_in_this_block / BIT_PER_BYTE; byte++)
		{
            for (uint32 shift = 0; shift < BIT_PER_BYTE; shift++)
			{
                if (current_bit >= total_bits)
					break;

                uint8 mask = (uint8)(1U << shift);
                if (buf->data[byte] & mask)
                    printf("%d ", global_base + current_bit);
                current_bit++;
            }
        }
        buffer_put(buf);
    }
	printf("over!\n\n");
}
