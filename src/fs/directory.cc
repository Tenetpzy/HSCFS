#include "cache/node_block_cache.hh"
#include "cache/dir_data_block_cache.hh"
#include "fs/directory.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "fs/file_utils.hh"
#include "utils/hscfs_log.h"

#include <tuple>

namespace hscfs {

dentry_info::dentry_info()
{
    ino = INVALID_NID;
}

dentry_handle directory::create(const std::string &name, uint8_t type, const dentry_store_pos *create_pos_hint)
{
    /* 注意，可能有同名的老dentry，但它处于deleted状态，此时不应该出错 */
    
}

dentry_info directory::lookup(const std::string &name)
{
    dentry_info target_info;
    node_block_cache_entry_handle inode_handle = node_cache_helper(fs_manager).get_node_entry(ino, INVALID_NID);
    assert(inode_handle->get_node_block_ptr()->footer.ino == ino);
    assert(inode_handle->get_node_block_ptr()->footer.nid == ino);
    hscfs_inode *inode = &inode_handle->get_node_block_ptr()->i;
    uint32_t cur_hash_level = inode->i_current_depth;
    u32 name_hash = hscfs_dentry_hash(name.c_str(), name.length());

    /* 依次遍历每一级哈希表中对应的桶 */
    for (uint32_t level = 0; level <= cur_hash_level; ++level)
    {
        uint32_t num_buckets = bucket_num(level, inode->i_dir_level);
        uint32_t bucket_idx_of_name = name_hash % num_buckets;

        /* 遍历桶内每一个block，查找目录项与可插入位置 */
        uint32_t num_bucket_block = bucket_block_num(level);
        uint32_t start_blkno = bucket_start_block_index(level, inode->i_dir_level, bucket_idx_of_name);
        uint32_t end_blkno = start_blkno + num_bucket_block;
        assert(end_blkno <= SIZE_TO_BLOCK(inode->i_size));

        for (uint32_t blkno = start_blkno; blkno < end_blkno; ++blkno)
        {
            dentry_info target_in_cur_blk = find_dentry_in_block(blkno, name, name_hash);

            if (target_in_cur_blk.ino != INVALID_NID)
            {
                target_info = target_in_cur_blk;
                break;
            }

            /* 将找到的第一个可创建位置记录下来 */
            if (!target_info.store_pos.is_valid && target_in_cur_blk.store_pos.is_valid)
                target_info.store_pos = target_in_cur_blk.store_pos;
        }

        /* 已经找到了，退出 */
        if (target_info.ino != INVALID_NID)
            break;
    }

    if (target_info.ino == INVALID_NID)
        HSCFS_LOG(HSCFS_LOG_INFO, "lookup: dentry [%s] not found in dir [%u].", name.c_str(), ino);
    else
        HSCFS_LOG(HSCFS_LOG_INFO, "lookup: dentry [%s] found in dir [%u].", name.c_str(), ino);
    
    return target_info;
}

block_buffer directory::create_formatted_data_block_buffer()
{
    /* block buffer使用spdk_zmalloc分配内存，该内存已经被初始化为0，不需要再格式化 */
    return block_buffer();
}

/* 目录项查找的辅助函数和结构 */
/************************************************************/
struct hscfs_dentry_ptr {
	void *bitmap;
	struct hscfs_dir_entry *dentry;
	u8 (*filename)[HSCFS_SLOT_LEN];
	int max;
	int nr_bitmap;
};

#define BITS_PER_LONG 64
#define BIT_WORD(nr)		((nr) / BITS_PER_LONG)

dentry_info directory::find_dentry_in_block(uint32_t blkno, const std::string &name, u32 name_hash) const
{
    HSCFS_LOG(HSCFS_LOG_INFO, "finding dentry [%s] in dir(ino=%u, blkno=%u).", name.c_str(), ino, blkno);
    dentry_info target_dentry_info;

    /* 获取blkno缓存 */
    block_addr_info blk_addr;
    dir_data_block_handle block_handle;
    std::tie(block_handle, blk_addr) = dir_data_cache_helper(fs_manager).get_dir_data_block(ino, blkno);

    /* 如果是文件空洞(block还未分配)，则返回该块的首个slot为可创建位置 */
    if (block_handle.is_empty())
    {
        HSCFS_LOG(HSCFS_LOG_INFO, "blkno [%u] is not allocated in dir [%u], dentry [%s] can be created here.",
            blkno, ino, name.c_str());
        assert(blk_addr.lpa == INVALID_LPA);
        target_dentry_info.ino = INVALID_NID;
        target_dentry_info.store_pos.set_pos(blkno, 0);
        return target_dentry_info;
    }

    /* 在blkno中查找目录项，若查不到，则获取最大空闲slot位置和数目 */
    hscfs_dentry_block *block = block_handle->get_block_ptr();
    hscfs_dentry_ptr d_meta;
    hscfs_dir_entry *tar_dentry = nullptr;
    int max_free_slot_num;
    uint32_t max_free_slot_pos;
    make_dentry_ptr_block(&d_meta, block);
    tar_dentry = hscfs_find_target_dentry(reinterpret_cast<const unsigned char*>(name.c_str()), name.length(),
    name_hash, &max_free_slot_num, &max_free_slot_pos, &d_meta);

    /* 设置目录项信息 */
    if (tar_dentry == nullptr)  // blkno中不存在目标目录项
    {
        target_dentry_info.ino = INVALID_NID;
        int slot_needed = GET_DENTRY_SLOTS(name.length());
        if (slot_needed <= max_free_slot_num)  // 找到了可以存储目标目录项的位置
        {
            target_dentry_info.store_pos.set_pos(blkno, max_free_slot_pos);
            HSCFS_LOG(HSCFS_LOG_INFO, "dentry [%s] not found in block [%u], but can be created at slot pos [%u].",
                name.c_str(), blkno, max_free_slot_pos);
        }
        else
            HSCFS_LOG(HSCFS_LOG_INFO, "dentry [%s] not found in block [%u], also no free pos to create in this block.",
                name.c_str(), blkno);
    }
    else  // blkno中存在目录项
    {
        target_dentry_info.ino = tar_dentry->ino;
        target_dentry_info.type = tar_dentry->file_type;
        target_dentry_info.store_pos.set_pos(blkno, tar_dentry - block->dentry);
        HSCFS_LOG(HSCFS_LOG_INFO, "dentry [%s] found in block [%u], at slot pos [%u].", name.c_str(), blkno, 
            target_dentry_info.store_pos.slotno);
    }
    return target_dentry_info;
}

uint32_t directory::bucket_num(u32 level, int dir_level)
{
	if (level + dir_level < MAX_DIR_HASH_DEPTH / 2)
		return 1 << (level + dir_level);
	else
		return MAX_DIR_BUCKETS;
}

u32 directory::bucket_block_num(u32 level)
{
    if (level < MAX_DIR_HASH_DEPTH / 2)
		return 2;
	else
		return 4;
}

u32 directory::bucket_start_block_index(u32 level, int dir_level, u32 bucket_idx)
{
	u32 i;
	u32 bidx = 0;

	for (i = 0; i < level; i++)
		bidx += bucket_num(i, dir_level) * bucket_block_num(i);
	bidx += bucket_idx * bucket_block_num(level);
	return bidx;
}

u32 directory::hscfs_dentry_hash(const char *name, u32 len)
{
    auto str2hashbuf = [](const unsigned char *msg, u32 len, u32 *buf, int num)
    {
        u32 pad, val;
        u32 i;

        pad = len | (len << 8);
        pad |= pad << 16;

        val = pad;
        if (len > (u32)num * 4)
            len = num * 4;
        for (i = 0; i < len; i++) {
            if ((i % 4) == 0)
                val = pad;
            val = msg[i] + (val << 8);
            if ((i % 4) == 3) {
                *buf++ = val;
                val = pad;
                num--;
            }
        }
        if (--num >= 0)
            *buf++ = val;
        while (--num >= 0)
            *buf++ = pad;
    };

    auto TEA_transform = [](u32 buf[4], u32 in[])
    {
        u32 sum = 0;
        u32 b0 = buf[0], b1 = buf[1];
        u32 a = in[0], b = in[1], c = in[2], d = in[3];
        int n = 16;

        do {
            sum += 0x9E3779B9;
            b0 += ((b1 << 4)+a) ^ (b1+sum) ^ ((b1 >> 5)+b);
            b1 += ((b0 << 4)+c) ^ (b0+sum) ^ ((b0 >> 5)+d);
        } while (--n);

        buf[0] += b0;
        buf[1] += b1;
    };

    u32 hash;
	u32 hscfs_hash;
	const unsigned char *p;
	u32 in[8], buf[4];

	if ((len == 1 && name[0] == '.') ||
        (len == 2 && name[0] == '.' && name[1] == '.'))
		return 0;

	/* Initialize the default seed for the hash checksum functions */
	buf[0] = 0x67452301;
	buf[1] = 0xefcdab89;
	buf[2] = 0x98badcfe;
	buf[3] = 0x10325476;

	p = reinterpret_cast<const unsigned char*>(name);
	while (1) {
		str2hashbuf(p, len, in, 4);
		TEA_transform(buf, in);
		p += 16;
		if (len <= 16)
			break;
		len -= 16;
	}
	hash = buf[0];
	hscfs_hash = hash & ~HSCFS_HASH_COL_BIT;
	return hscfs_hash;
}

void directory::make_dentry_ptr_block(hscfs_dentry_ptr *d, hscfs_dentry_block *t)
{
	d->max = NR_DENTRY_IN_BLOCK;
	d->nr_bitmap = SIZE_OF_DENTRY_BITMAP;
	d->bitmap = t->dentry_bitmap;
	d->dentry = t->dentry;
	d->filename = t->filename;
}

hscfs_dir_entry *directory::hscfs_find_target_dentry(const unsigned char *name, u32 nameLen, uint32_t namehash, 
    int *max_slots, u32 *empty_bit_pos, hscfs_dentry_ptr *d)
{
    auto test_bit_le = [](unsigned long nr, const void *addr)
    {
        return (u64)1 & (((const u64 *)addr)[BIT_WORD(nr)] >> (nr & (BITS_PER_LONG - 1)));
    };

	hscfs_dir_entry *de;
	unsigned long bit_pos = 0;
	int max_len = 0;

	if (max_slots)
		*max_slots = 0;
	while (bit_pos < (u32)d->max) {
		if (!test_bit_le(bit_pos, d->bitmap)) {
			bit_pos++;
			max_len++;
			continue;
		}

		de = &d->dentry[bit_pos];

		if (!de->name_len) {
			bit_pos++;
			continue;
		}

		if (hscfs_match_name(d, de, name, nameLen, bit_pos, namehash))
			goto found;

		if (max_slots && max_len > *max_slots)
		{
			*max_slots = max_len;
			if(empty_bit_pos)
				*empty_bit_pos = bit_pos - max_len;
		}
		max_len = 0;

		bit_pos += GET_DENTRY_SLOTS(de->name_len);
	}

	de = NULL;
found:
	if (max_slots && max_len > *max_slots)
	{
		*max_slots = max_len;
		if(empty_bit_pos)
			*empty_bit_pos = bit_pos - max_len;
	}

	return de;
}

u32 directory::hscfs_match_name(hscfs_dentry_ptr *d, hscfs_dir_entry *de, const unsigned char *name, u32 len, 
    unsigned long bit_pos, hscfs_hash_t namehash)
{
    auto compare_name = [](const unsigned char* name1, const unsigned char* name2, u32 len)
    {
        for(u32 i = 0; i < len; ++i)
        {
            if(name1[i] != name2[i])
                return 0;
        }
        return 1;
    };
	if(de->hash_code != namehash)
		return 0;
    if(len != de->name_len)
		return 0;
	if(compare_name(name, (const unsigned char*)d->filename[bit_pos], de->name_len))
		return 1;
	return 0;
}

} // namespace hscfs