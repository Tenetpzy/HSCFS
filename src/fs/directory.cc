#include "cache/node_block_cache.hh"
#include "cache/dir_data_block_cache.hh"
#include "cache/node_block_cache.hh"
#include "fs/directory.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "fs/file_utils.hh"
#include "utils/hscfs_log.h"

#include <tuple>
#include <cstring>

namespace hscfs {

dentry_info::dentry_info()
{
    ino = INVALID_NID;
}

dentry_handle directory::create(const std::string &name, uint8_t type, const dentry_store_pos *create_pos_hint)
{
    /* 
     * 注意：可能有同名的老dentry，但它处于deleted状态，此时不应该出错
     * 调用者提供的create_pos_hint可能是不正确的（如果此目录添加过文件，且这个位置是SSD返回）
     */
    
    dentry_store_pos create_pos;  // 最终决定的目录项创建位置
    bool create_pos_hint_valid = true;
    dir_data_cache_helper dir_data_helper(fs_manager);
    node_cache_helper node_helper(fs_manager);
    auto inode_handle = node_helper.get_node_entry(ino, INVALID_NID);
    hscfs_inode *inode = &inode_handle->get_node_block_ptr()->i;
    assert(inode->i_size % 4096 == 0);

    /* 首先要检查create_pos_hint是否有效 */
    if (create_pos_hint == nullptr || !create_pos_hint->is_valid)
        create_pos_hint_valid = false;
    else
    {
        uint32_t max_blk_off = SIZE_TO_BLOCK(inode->i_size) - 1;  // 计算当前目录文件的最大块偏移

        /* 
        * 如果create_pos_hint在文件范围内，则检查该位置是否确实可用
        * 如果在文件范围外，则不可用（确保接下来的步骤中，能够正确地新建哈希表）
        */
        create_pos_hint_valid = is_create_pos_valid(name, create_pos_hint, max_blk_off);
    }

    /* 如果调用者传入的创建位置无效，则搜索可创建位置 */
    if (!create_pos_hint_valid)
    {
        dentry_info info = lookup(name);
        assert(info.ino == INVALID_NID);
        
        /* 如果当前目录文件找不到可创建位置了，则新增一个哈希表 */
        if (!info.store_pos.is_valid)
        {
            HSCFS_LOG(HSCFS_LOG_DEBUG, "directory(ino = %u) has no space to create dentry %s.", ino, name.c_str());
            append_hash_level(inode);

            /* 新增哈希表之后，再次进行lookup，一定能找到创建位置 */
            info = lookup(name);
            assert(info.ino == INVALID_NID);
            assert(info.store_pos.is_valid == true);
        }

        create_pos = info.store_pos;
    }

    /* 如果调用者提供的位置有效，则直接使用该位置 */
    else
        create_pos = *create_pos_hint;
    
    /* 此时，create_pos是有效的，但需要判断是否为文件空洞 */
    assert(create_pos.is_valid == true);
    HSCFS_LOG(HSCFS_LOG_INFO, "create dentry %s in directory(ino = %u), pos: blkno = %u, slotno = %u",
        name.c_str(), ino, create_pos.blkno, create_pos.slotno);

    dir_data_block_handle create_blk_handle;
    std::tie(create_blk_handle, std::ignore) = dir_data_helper.get_dir_data_block(ino, create_pos.blkno);

    /* 如果是文件空洞，则创建一个数据块缓存（此时不分配地址，提交时再分配） */
    if (create_blk_handle.is_empty())
    {
        create_blk_handle = fs_manager->get_dir_data_cache()->add(ino, create_pos.blkno, INVALID_LPA, 
            create_formatted_data_block_buffer());
        create_blk_handle.mark_dirty();
        HSCFS_LOG(HSCFS_LOG_INFO, "creation pos is in file hole, allocated a dir data block buffer for it.");
    }

    /* 此时，create_blk_handle指向待创建目录项的数据块 */
    /* 分配一个inode */
    node_block_cache_entry_handle new_inode_handle;
    if (type == HSCFS_FT_REG_FILE)
        new_inode_handle = file_creator(fs_manager).create_generic_file();
    else
    {
        assert(type == HSCFS_FT_DIR);
        new_inode_handle = file_creator(fs_manager).create_directory();
    }
    uint32_t new_inode = new_inode_handle->get_nid();
    create_dentry_in_blk(name, type, new_inode, create_blk_handle, create_pos);

    /* to do: 将新目录项加入dentry cache */
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

bool directory::test_bitmap_pos(unsigned long slot_pos, const void *bitmap_start_addr)
{
    return (u64)1 & (((const u64 *)bitmap_start_addr)[BIT_WORD(slot_pos)] >> (slot_pos & (BITS_PER_LONG - 1)));
}

hscfs_dir_entry *directory::hscfs_find_target_dentry(const unsigned char *name, u32 nameLen, uint32_t namehash,
                                                     int *max_slots, u32 *empty_bit_pos, hscfs_dentry_ptr *d)
{
	hscfs_dir_entry *de;
	unsigned long bit_pos = 0;
	int max_len = 0;

	if (max_slots)
		*max_slots = 0;
	while (bit_pos < (u32)d->max) {
		if (!test_bitmap_pos(bit_pos, d->bitmap)) {
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

bool directory::is_create_pos_valid(const std::string &name, const dentry_store_pos *create_pos_hint, uint32_t max_blk_off)
{
    if (create_pos_hint->blkno > max_blk_off)
        return false;
    dir_data_cache_helper dir_data_helper(fs_manager);
    dir_data_block_handle create_blk_handle;
    block_addr_info create_blk_addr;
    std::tie(create_blk_handle, create_blk_addr) = dir_data_helper.get_dir_data_block(ino, create_pos_hint->blkno);

    /* 如果create_pos_hint不是文件空洞，则需要检查 */
    if (!create_blk_handle.is_empty())
    {
        hscfs_dentry_block *create_blk = create_blk_handle->get_block_ptr();
        uint8_t *bitmap_addr = create_blk->dentry_bitmap;
        size_t name_occupy_slots = GET_DENTRY_SLOTS(name.length());
        for (uint32_t i = 0; i < name_occupy_slots; ++i)
        {
            if (test_bitmap_pos(i + create_pos_hint->slotno, bitmap_addr))
            {
                return false;
            }
        }
    }

    return true;
}

void directory::append_hash_level(hscfs_inode *inode)
{
    ++inode->i_current_depth;
    uint32_t blknum_need_expand = bucket_num(inode->i_current_depth, inode->i_dir_level) *
        bucket_block_num(inode->i_current_depth);
    uint64_t size_after_expand = (SIZE_TO_BLOCK(inode->i_size) + blknum_need_expand) * 4096;
    HSCFS_LOG(HSCFS_LOG_DEBUG, "appending directory(ino = %u) hash level to %u, need append another %u blocks.", 
        ino, inode->i_current_depth, blknum_need_expand);
    file_resizer(fs_manager).expand(ino, size_after_expand);  // 会将inode标记位dirty
}

void directory::set_bitmap_pos(unsigned long slot_pos, void *bitmap_start_addr)
{
    auto start_addr = static_cast<uint64_t*>(bitmap_start_addr);
    uint64_t idx = BIT_WORD(slot_pos);
    uint64_t off = slot_pos & (BITS_PER_LONG - 1U);
    start_addr[idx] |= (1U << off);
}

void directory::create_dentry_in_blk(const std::string &name, uint8_t type, uint32_t ino,
    dir_data_block_handle blk_handle, const dentry_store_pos &pos)
{
    size_t name_len = name.length();
    assert(name_len != 0);

    uint32_t start_slot = pos.slotno;
    uint32_t occupy_slot_num = GET_DENTRY_SLOTS(name_len);
    uint32_t hash_code = hscfs_dentry_hash(name.c_str(), name_len);

    hscfs_dentry_block *blk = blk_handle->get_block_ptr();
    uint8_t *name_store_addr = blk->filename[start_slot];
    uint8_t *bitmap_addr = blk->dentry_bitmap;
    hscfs_dir_entry *dentry_ptr = &blk->dentry[start_slot];

    /* 设置位图 */
    for (uint32_t i = 0; i < occupy_slot_num; ++i)
    {
        assert(!test_bitmap_pos(start_slot + i, bitmap_addr));
        set_bitmap_pos(start_slot + i, bitmap_addr);
        assert(test_bitmap_pos(start_slot + i, bitmap_addr));
    }

    /* 设置目录项 */
    dentry_ptr->hash_code = hash_code;
    dentry_ptr->file_type = type;
    dentry_ptr->ino = ino;
    dentry_ptr->name_len = name_len;

    /* 设置文件名 */
    std::memcpy(name_store_addr, name.c_str(), name_len);

    /* 标记为dirty */
    blk_handle.mark_dirty();

    HSCFS_LOG(HSCFS_LOG_INFO, "construct dentry %s in dir data block, hash = %#x, occupied slot num = %u.", 
        name.c_str(), hash_code, occupy_slot_num);
}

} // namespace hscfs