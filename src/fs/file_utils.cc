#include "fs/file_utils.hh"
#include "fs/file.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "fs/NAT_utils.hh"
#include "fs/SIT_utils.hh"
#include "fs/directory.hh"
#include "fs/replace_protect.hh"
#include "cache/node_block_cache.hh"
#include "communication/memory.h"
#include "communication/comm_api.h"
#include "utils/hscfs_log.h"
#include "utils/hscfs_exceptions.hh"
#include "utils/dma_buffer_deletor.hh"

#include <memory>
#include <tuple>

struct comm_dev;

namespace hscfs {

/* SSD file mapping查询控制器 */
class ssd_file_mapping_search_controller
{
public:
	ssd_file_mapping_search_controller(file_system_manager *fs_manager)
	{
		this->fs_manager = fs_manager;
		dev = fs_manager->get_device();
	}

	/* 构造命令和返回值的buffer */
	void construct_task(uint32_t ino, uint32_t nid_to_start, uint32_t blkno, uint32_t level_num);

	/* 发送filemapping查询命令，将结果保存到内部buf中。同步，等待命令执行完成后返回 */
	void do_filemapping_search();

	hscfs_node* get_start_addr_of_result() const noexcept
	{
		return p_task_res_buf.get();
	}

private:
	file_system_manager *fs_manager;
	comm_dev *dev;
	std::unique_ptr<filemapping_search_task, dma_buf_deletor> p_task_buf;
	std::unique_ptr<hscfs_node, dma_buf_deletor> p_task_res_buf;
	uint32_t level_num;
};

#ifdef CONFIG_PRINT_DEBUG_INFO
void print_filemapping_search_task(filemapping_search_task *task);
void print_filemapping_search_result(hscfs_node *node, uint32_t level_num);
#endif

void ssd_file_mapping_search_controller::construct_task(uint32_t ino, uint32_t nid_to_start, uint32_t blkno, 
	uint32_t level_num)
{
	void *buf = comm_alloc_dma_mem(sizeof(filemapping_search_task));
	if (buf == nullptr)
		throw alloc_error("ssd_file_mapping_search_controller: alloc task memory failed.");
	p_task_buf.reset(static_cast<filemapping_search_task*>(buf));

	/* 默认返回每一级 */
	buf = comm_alloc_dma_mem(level_num * sizeof(hscfs_node));
	if (buf == nullptr)
		throw alloc_error("ssd_file_mapping_search_controller: alloc task result memory failed.");
	p_task_res_buf.reset(static_cast<hscfs_node*>(buf));

	p_task_buf->ino = ino;
	p_task_buf->nid_to_start = nid_to_start;
	p_task_buf->file_blk_offset = blkno;
	p_task_buf->return_all_Level = 1;

	this->level_num = level_num;

	#ifdef CONFIG_PRINT_DEBUG_INFO
	print_filemapping_search_task(p_task_buf.get());
	#endif
}

void ssd_file_mapping_search_controller::do_filemapping_search()
{
	/* 需要等待SSD侧日志应用完成 */
	replace_protect_manager *rp_manager = fs_manager->get_replace_protect_manager();
	rp_manager->wait_all_journal_applied_in_SSD();

	assert(p_task_buf != nullptr && p_task_res_buf != nullptr);
	int ret = comm_submit_sync_filemapping_search_request(dev, p_task_buf.get(), p_task_res_buf.get(), 
		level_num * sizeof(hscfs_node));
	if (ret != 0)
		throw io_error("ssd_file_mapping_search_controller: send file mapping search task failed.");

	#ifdef CONFIG_PRINT_DEBUG_INFO
	print_filemapping_search_result(p_task_res_buf.get(), level_num);
	#endif
}

/*********************************************************************/

int file_mapping_util::get_node_path(uint64_t block, uint32_t offset[4], uint32_t noffset[4])
{
	const long direct_index = DEF_ADDRS_PER_INODE;
	const long direct_blks = DEF_ADDRS_PER_BLOCK;
	const long dptrs_per_blk = NIDS_PER_BLOCK;
	const long indirect_blks = DEF_ADDRS_PER_BLOCK * NIDS_PER_BLOCK;
	const long dindirect_blks = indirect_blks * NIDS_PER_BLOCK;
	int n = 0;
	int level = 0;

	noffset[0] = 0;

	if (block < direct_index) {
		offset[n] = block;
		goto got;
	}
	block -= direct_index;
	if (block < direct_blks) {
		offset[n++] = NODE_DIR1_BLOCK;
		noffset[n] = 1;
		offset[n] = block;
		level = 1;
		goto got;
	}
	block -= direct_blks;
	if (block < direct_blks) {
		offset[n++] = NODE_DIR2_BLOCK;
		noffset[n] = 2;
		offset[n] = block;
		level = 1;
		goto got;
	}
	block -= direct_blks;
	if (block < indirect_blks) {
		offset[n++] = NODE_IND1_BLOCK;
		noffset[n] = 3;
		offset[n++] = block / direct_blks;
		noffset[n] = 4 + offset[n - 1];
		offset[n] = block % direct_blks;
		level = 2;
		goto got;
	}
	block -= indirect_blks;
	if (block < indirect_blks) {
		offset[n++] = NODE_IND2_BLOCK;
		noffset[n] = 4 + dptrs_per_blk;
		offset[n++] = block / direct_blks;
		noffset[n] = 5 + dptrs_per_blk + offset[n - 1];
		offset[n] = block % direct_blks;
		level = 2;
		goto got;
	}
	block -= indirect_blks;
	if (block < dindirect_blks) {
		offset[n++] = NODE_DIND_BLOCK;
		noffset[n] = 5 + (dptrs_per_blk * 2);
		offset[n++] = block / indirect_blks;
		noffset[n] = 6 + (dptrs_per_blk * 2) +
			      offset[n - 1] * (dptrs_per_blk + 1);
		offset[n++] = (block / direct_blks) % dptrs_per_blk;
		noffset[n] = 7 + (dptrs_per_blk * 2) +
			      offset[n - 2] * (dptrs_per_blk + 1) +
			      offset[n - 1];
		offset[n] = block % direct_blks;
		level = 3;
		goto got;
	} else {
		return -1;
	}
got:
	return level;
}

uint32_t file_mapping_util::get_next_nid(hscfs_node *node, uint32_t offset, int cur_level)
{
    if (cur_level == 0)  // 如果是inode
		return node->i.i_nid[offset - NODE_DIR1_BLOCK];
	return node->in.nid[offset];
}

void file_mapping_util::set_next_nid(hscfs_node *node, uint32_t offset, int cur_level, uint32_t nxt_nid)
{
    if (cur_level == 0)
		node->i.i_nid[offset - NODE_DIR1_BLOCK] = nxt_nid;
	else
		node->in.nid[offset] = nxt_nid;
}

uint32_t file_mapping_util::get_lpa(hscfs_node *node, uint32_t offset, int level)
{
	/* 如果是inode */
    if (level == 0)
	{
		assert(offset < DEF_ADDRS_PER_INODE);  // offset应在i_addr数组范围内
		return node->i.i_addr[offset];
	}
	else  // 否则是direct node
	{
		assert(offset < DEF_ADDRS_PER_BLOCK);  // offset应在addr数组范围内
		return node->dn.addr[offset];
	}
}

void file_mapping_util::set_lpa(hscfs_node *node, uint32_t offset, int level, uint32_t lpa)
{
	/* 如果是inode */
    if (level == 0)
	{
		assert(offset < DEF_ADDRS_PER_INODE);  // offset应在i_addr数组范围内
		node->i.i_addr[offset] = lpa;
	}
	else  // 否则是direct node
	{
		assert(offset < DEF_ADDRS_PER_BLOCK);  // offset应在addr数组范围内
		node->dn.addr[offset] = lpa;
	}
}

block_addr_info file_mapping_util::get_addr_of_block(uint32_t ino, uint32_t blkno)
{
    uint32_t offset[4], noffset[4];
    int level = get_node_path(blkno, offset, noffset);
    assert(level != -1);
	HSCFS_LOG(HSCFS_LOG_INFO, "file_mapping_util: start file mapping search: "
		"target inode: %u, target block offset: %u, target path level in node tree: %d",
		ino, blkno, level
	);

    node_block_cache *node_cache = fs_manager->get_node_cache();

	hscfs_node *last_node = nullptr;  // 拥有指向blkno的direct point的node
	uint32_t cur_nid = ino;  // 当前查找的nid
	node_block_cache_entry_handle cur_handle;  // 当前nid的缓存项handle
	uint32_t parent_nid = INVALID_NID;  // cur_nid的父nid
	int cur_level = 0;  // 当前查找的路径层级(inode为0，子结点递增)

    while (true)
    {
        cur_handle = node_cache->get(cur_nid);

        /* 当前node block缓存不命中，交给SSD进行查询 */
        if (cur_handle.is_empty())
        {
			HSCFS_LOG(HSCFS_LOG_INFO, "file_mapping_util:"
				"node block[file(inode: %u), level %d, nid %u] miss. Prepare fetching from SSD", 
				ino, cur_level, cur_nid);
			int ssd_level = level - cur_level + 1;
			ssd_file_mapping_search_controller ctrlr(fs_manager);
			ctrlr.construct_task(ino, cur_nid, blkno, ssd_level);
			ctrlr.do_filemapping_search();

			/* 将结果插入node cache */
			hscfs_node *p_node = ctrlr.get_start_addr_of_result();
			uint32_t parent = parent_nid;
			for (int i = 0; i < ssd_level; ++i)
			{
				/* 得到当前node page的nid和lpa */
				uint32_t nid = p_node->footer.nid;
				uint32_t lpa = nat_lpa_mapping(fs_manager).get_lpa_of_nid(nid);
				
				block_buffer buffer;
				buffer.copy_content_from_buf(reinterpret_cast<char*>(p_node));
				
				/* 此时parent一定在缓存中，直接插入不会出错 */
				node_cache->add(std::move(buffer), nid, parent, lpa);

				parent = nid;
				++p_node;
			}

			/* 此时不命中的node已全部插入node cache，继续外层查找 */
			continue;
        }

		/* 到此处，node cache命中，cur_handle有效 */
		last_node = cur_handle->get_node_block_ptr();
		assert(last_node->footer.ino == ino);
		assert(last_node->footer.nid == cur_nid);
		assert(last_node->footer.offset == noffset[cur_level]);

		/* 已经找到索引路径最后一级node page，退出 */
		if (cur_level == level)
			break;

		/* 否则，继续查找路径上的下一个node page */
		uint32_t nxt_nid = get_next_nid(last_node, offset[cur_level], cur_level);
		HSCFS_LOG(HSCFS_LOG_INFO, "file mapping searcher: searching in level %d, nid %u, offset %u. "
			"next nid is %u.", cur_level, cur_nid, offset[cur_level], nxt_nid);
		++cur_level;
		parent_nid = cur_nid;
		cur_nid = nxt_nid;
    }

	/* 到此处，已查找到索引树最后一级node page */
	assert(last_node != nullptr);
	uint32_t target_lpa = get_lpa(last_node, offset[level], level);
	HSCFS_LOG(HSCFS_LOG_INFO, "file mapping searcher: reach search path end. nid: %u, level: %u, " 
		"direct pinter offset: %u, target lpa: %u.", cur_nid, level, offset[level], target_lpa);
	
	block_addr_info ret;
	ret.lpa = target_lpa;
	ret.nid = cur_nid;
	ret.nid_off = offset[level];
	ret.nid_handle = std::move(cur_handle);
	return ret;
}

block_addr_info file_mapping_util::update_block_mapping(uint32_t ino, uint32_t blkno, uint32_t new_lpa)
{
	block_addr_info addr = get_addr_of_block(ino, blkno);
	assert(addr.lpa != new_lpa);
	assert(addr.nid_handle->get_nid() == addr.nid);

	uint32_t level = ino == addr.nid ? 0 : 1;
	hscfs_node *node = addr.nid_handle->get_node_block_ptr();
	set_lpa(node, addr.nid_off, level, new_lpa);
	addr.nid_handle.mark_dirty();
	addr.lpa = new_lpa;

	return addr;
}

/******************************************************************************/

const uint32_t file_resizer::single_node_blks = DEF_ADDRS_PER_BLOCK;
const uint32_t file_resizer::double_node_blks = NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
const uint32_t file_resizer::triple_node_blks = NIDS_PER_BLOCK * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;
const uint64_t file_resizer::max_blkno_limit = DEF_ADDRS_PER_INODE + 2 * DEF_ADDRS_PER_BLOCK + 
	2 * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK +
	NIDS_PER_BLOCK * NIDS_PER_BLOCK * DEF_ADDRS_PER_BLOCK;

void file_resizer::reduce(uint32_t ino, uint64_t tar_size)
{
	/* 获取该文件的inode block */
	node_cache_helper node_helper(fs_manager);
	node_block_cache_entry_handle inode_handle = node_helper.get_node_entry(ino, INVALID_NID);
	hscfs_node *node = inode_handle->get_node_block_ptr();
	assert(node->footer.ino == node->footer.nid);
	assert(node->footer.offset == 0);
	hscfs_inode *inode = &node->i;

	/* 更新inode中的size */
	uint64_t cur_size = inode->i_size;
	if (cur_size <= tar_size)  return;
	inode->i_size = tar_size;
	inode_handle.mark_dirty();

	/* 
	 * 计算需要释放的起止块偏移闭区间[start_blk, end_blk]
	 * SIZE_TO_BLOCK返回的是大小对应的块数量，-1才是块偏移 
	 */
	uint32_t start_blk = SIZE_TO_BLOCK(tar_size);  // start_blk = start block index + 1
	uint32_t end_blk = SIZE_TO_BLOCK(cur_size) - 1;  // end_blk = end block index - 1
	if (start_blk > end_blk)  // reduce后，仍然在同一个块内
	{
		assert(start_blk == end_blk + 1);
		return;
	}
	
	HSCFS_LOG(HSCFS_LOG_INFO, "reduce file [%u] size from %u bytes to %u bytes, will free blocks ranging [%u, %u].",
		ino, cur_size, tar_size, start_blk, end_blk);
	free_blocks_in_range(node, start_blk, end_blk);
}

void file_resizer::expand(uint32_t ino, uint64_t tar_size)
{
	/* 获取该文件的inode */
	node_cache_helper node_helper(fs_manager);
	node_block_cache_entry_handle inode_handle = node_helper.get_node_entry(ino, INVALID_NID);
	hscfs_node *node = inode_handle->get_node_block_ptr();
	assert(node->footer.ino == node->footer.nid);
	assert(node->footer.offset == 0);
	hscfs_inode *inode = &node->i;

	/* 更新inode中的size */
	uint64_t cur_size = inode->i_size;
	if (tar_size <= cur_size)  return;
	if (SIZE_TO_BLOCK(tar_size) > max_blkno_limit)
		throw expand_file_size_exceed_limit("expand file size exceeding limit.");
	inode->i_size = tar_size;
	inode_handle.mark_dirty();

	/* 计算需要增加的起止块偏移闭区间[start_blk, end_blk] */
	uint32_t start_blk = SIZE_TO_BLOCK(cur_size);
	uint32_t end_blk = SIZE_TO_BLOCK(tar_size) - 1;
	if (start_blk > end_blk)  // expand后，仍然在同一块内
	{
		assert(start_blk == end_blk + 1);
		return;
	}

	HSCFS_LOG(HSCFS_LOG_INFO, "expand file [%u] size from %u bytes to %u bytes, will add blocks ranging [%u, %u].", 
		ino, cur_size, tar_size, start_blk, end_blk);

	/* 对每一个新增的block，确保索引它的node存在 */
	for (uint32_t blk = start_blk; blk <= end_blk; ++blk)
	{
		block_node_path blk_path;
		blk_path.level = file_mapping_util::get_node_path(blk, blk_path.offset, blk_path.noffset);
		assert(blk_path.level != -1);

		/* 沿着当前block的索引路径向下，如果遇到INVALID_NID，则创建该node block */
		/* cur_nid, cur_handle：当前处理的node。*/
		uint32_t cur_nid = ino;
		node_block_cache_entry_handle cur_handle = inode_handle;
		for (int level = 0; level < blk_path.level; ++level)
		{
			hscfs_node *cur_node = cur_handle->get_node_block_ptr();
			uint32_t nxt_nid = file_mapping_util::get_next_nid(cur_node, blk_path.offset[level], level);

			/* 下一级索引不存在，创建它 */
			if (nxt_nid == INVALID_NID)
			{
				node_block_cache_entry_handle nxt_handle = node_helper.create_node_entry(ino, 
					blk_path.noffset[level + 1], cur_nid);
				nxt_nid = nxt_handle->get_nid();

				/* 修改当前node中指向下一级的索引，并标记为dirty */
				file_mapping_util::set_next_nid(cur_node, blk_path.offset[level], level, nxt_nid);
				cur_handle.mark_dirty();

				HSCFS_LOG(HSCFS_LOG_DEBUG, "next node level of [node: %u, offset: %u] didn't exist, "
					"created with new nid %u.", cur_nid, blk_path.offset[level], nxt_nid);
				
				cur_handle = nxt_handle;
			}

			/* 下一级索引项存在，从缓存中或SSD中读取 */
			else
			{
				cur_handle = node_helper.get_node_entry(nxt_nid, cur_nid);
			}

			cur_nid = nxt_nid;
			assert(cur_handle->get_nid() == cur_nid);
			assert(cur_handle->get_node_block_ptr()->footer.ino == ino);
			assert(cur_handle->get_node_block_ptr()->footer.nid == cur_nid);
			assert(cur_handle->get_node_block_ptr()->footer.offset == blk_path.noffset[level + 1]);
		}
	}
}

void file_resizer::free_blocks_in_range(hscfs_node *inode, const uint32_t start_blk, const uint32_t end_blk)
{
	auto intersect = [](uint32_t start1, uint32_t end1, uint32_t start2, uint32_t end2) 
		-> std::tuple<bool, uint32_t, uint32_t>
	{
		if (end1 < start2 || start1 > end2)
			return std::make_tuple(false, 0, 0);
		return std::make_tuple(true, std::max(start1, start2), std::min(end1, end2));
	};

	/* 处理inode中的direct pointers，将范围内的置为INVALID_LPA */
	auto proc_direct_pointers_in_inode = [this, intersect, start_blk, end_blk](hscfs_node *inode)
	{
		uint32_t inode_start = 0, inode_end = DEF_ADDRS_PER_INODE - 1;
		uint32_t start, end;
		bool is_intersect;
		std::tie(is_intersect, start, end) = intersect(inode_start, inode_end, start_blk, end_blk);
		if (is_intersect)  // 相交，块交集是[start, end]
		{
			HSCFS_LOG(HSCFS_LOG_INFO, "invalid blocks [%u, %u] in inode.", start, end);
			SIT_operator sit_operator(fs_manager);
			for (uint32_t i = start; i <= end; ++i)
			{
				uint32_t &lpa = inode->i.i_addr[i];
				if (lpa != INVALID_LPA)
				{
					HSCFS_LOG(HSCFS_LOG_INFO, "block [%u] is valid(lpa = %u), will marked garbage.", i, lpa);
					sit_operator.invalidate_lpa(lpa);
					lpa = INVALID_LPA;
				}
			}
		}
	};

	/* 
	 * 处理single node中的direct pointers，将范围内的置为INVALID_LPA
	 * nid为该single node的id，parent_nid为该single node的parent 
	 * manage_start为该single node所管理的起始块偏移
	 * 返回该single node是否已经无效（管理的首个块范围超过start_blk，则无效。无效返回true）
	 */
	auto proc_single_node = [this, intersect, start_blk, end_blk](node_block_cache_entry_handle &handle, 
		uint32_t manage_start) -> bool
	{
		uint32_t manage_end = manage_start + single_node_blks - 1;
		uint32_t start_off, end_off;
		bool is_intersect;
		std::tie(is_intersect, start_off, end_off) = intersect(start_blk, end_blk, manage_start, manage_end);
		if (!is_intersect)
			return 0;

		uint32_t nid = handle->get_node_block_ptr()->footer.nid;
		HSCFS_LOG(HSCFS_LOG_INFO, "single node [%u], noffset [%u], manage block range: [%u, %u], "
			"Invalid block range [%u, %u].", 
			nid, handle->get_node_block_ptr()->footer.offset, manage_start, manage_end, start_off, end_off);

		start_off -= manage_start;
		end_off -= manage_start;
		direct_node *s_node = &handle->get_node_block_ptr()->dn;

		/* 此时，[start_off, end_off]为node内addr数组中，待处理的下标范围 */
		SIT_operator sit_operator(fs_manager);
		for (uint32_t i = start_off; i <= end_off; ++i)
		{
			uint32_t &lpa = s_node->addr[i];
			if (lpa != INVALID_LPA)
			{
				HSCFS_LOG(HSCFS_LOG_INFO, "block [%u] is valid(lpa = %u), will marked garbage.", 
					i + manage_start, lpa);
				sit_operator.invalidate_lpa(lpa);
				lpa = INVALID_LPA;
			}
		}

		bool invalid = manage_start >= start_blk;
		if (invalid)
			HSCFS_LOG(HSCFS_LOG_INFO, "single node [%u] is invalid now, will be deleted later.", nid);
		return invalid;
	};

	/* 
	 * 处理double node所管理的single node。
	 * 如果一个single node不再有效，将其删除，释放nid 
	 * nid为该double node的nid，parent_nid为其parent
	 * manage_start为该double node所管理的起始块偏移
	 * 返回double node是否无效。无效返回true
	 */
	auto proc_double_node = [this, intersect, proc_single_node, start_blk, end_blk](
		node_block_cache_entry_handle &handle, const uint32_t manage_start) -> bool
	{
		indirect_node *d_node = &handle->get_node_block_ptr()->in;
		uint32_t nid = handle->get_node_block_ptr()->footer.nid;

		uint32_t single_manage_start = manage_start;  // 当前处理的single node索引的起始块偏移
		for (uint32_t i = 0; i < NIDS_PER_BLOCK; ++i)
		{
			uint32_t single_manage_end = single_manage_start + single_node_blks - 1;
			if (single_manage_end < start_blk)  
				continue;
			if (single_manage_start > end_blk)
			{
				/* 当前single node已经超过了文件范围，则该single node不应该存在 */
				assert(d_node->nid[i] == INVALID_NID);
				break;
			}

			assert(d_node->nid[i] != INVALID_NID);
			node_block_cache_entry_handle single_handle = node_cache_helper(this->fs_manager)
				.get_node_entry(d_node->nid[i], nid);

			/* 如果该single block已经无效 */
			if (proc_single_node(single_handle, single_manage_start))
			{
				single_handle.delete_node();
				d_node->nid[i] = INVALID_NID;
			}
			else  // 否则，single block修改了一些索引项，标记为dirty
				single_handle.mark_dirty();
			single_manage_start += single_node_blks;
		}

		bool invalid = manage_start >= start_blk;
		if (invalid)
			HSCFS_LOG(HSCFS_LOG_INFO, "double node [%u] is invalid now, will be deleted later.", nid);
		return invalid;
	};

	/* 参数和返回值含义与proc_double_node相同。如果下属double node不再有效，则将其删除 */
	auto proc_triple_node = [this, intersect, proc_double_node, start_blk, end_blk](
		node_block_cache_entry_handle &handle, const uint32_t manage_start) -> bool
	{
		indirect_node *t_node = &handle->get_node_block_ptr()->in;
		uint32_t nid = handle->get_node_block_ptr()->footer.nid;

		uint32_t double_manage_start = manage_start;
		for (uint32_t i = 0; i < NIDS_PER_BLOCK; ++i)
		{
			uint32_t double_manage_end = double_manage_start + double_node_blks - 1;
			if (double_manage_end < start_blk)  
				continue;
			if (double_manage_start > end_blk)
			{
				assert(t_node->nid[i] == INVALID_NID);
				break;
			}

			assert(t_node->nid[i] != INVALID_NID);
			node_block_cache_entry_handle double_handle = node_cache_helper(this->fs_manager)
				.get_node_entry(t_node->nid[i], nid);
			if (proc_double_node(double_handle, double_manage_start))
			{
				double_handle.delete_node();
				t_node->nid[i] = INVALID_NID;
			}
			else
				double_handle.mark_dirty();
			double_manage_start += double_node_blks;
		}
		bool invalid = manage_start >= start_blk;
		if (invalid)
			HSCFS_LOG(HSCFS_LOG_INFO, "triple node [%u] is invalid now, will be deleted later.", nid);
		return invalid;
	};

	/* 处理inode中的direct pointers */
	proc_direct_pointers_in_inode(inode);

	/* 处理两个single node维护的block范围 */
	uint32_t cur_start = DEF_ADDRS_PER_INODE;
	uint32_t nid_idx = 0;
	for (; nid_idx < 2; ++nid_idx)
	{
		uint32_t cur_end = cur_start + single_node_blks - 1;
		if (cur_end < start_blk)  continue;
		if (cur_start > end_blk)  return;
		node_block_cache_entry_handle handle = node_cache_helper(this->fs_manager)
			.get_node_entry(inode->i.i_nid[nid_idx], inode->footer.ino);
		/* 如果该single node维护的block全部被删除，则删除该single node */
		if (proc_single_node(handle, cur_start))
		{
			inode->i.i_nid[nid_idx] = INVALID_NID;
			handle.delete_node();
		}
		else  /* 否则，该single node还存在，但是被修改了，标记为dirty */
			handle.mark_dirty();
		cur_start += single_node_blks;
	}

	/* 处理两个double node维护的block范围 */
	for (; nid_idx < 4; ++nid_idx)
	{
		uint32_t cur_end = cur_start + double_node_blks - 1;
		if (cur_end < start_blk)  continue;
		if (cur_start > end_blk)  return;
		node_block_cache_entry_handle handle = node_cache_helper(this->fs_manager)
			.get_node_entry(inode->i.i_nid[nid_idx], inode->footer.ino);
		if (proc_double_node(handle, cur_start))
		{
			inode->i.i_nid[nid_idx] = INVALID_NID;
			handle.delete_node();
		}
		else
			handle.mark_dirty();
		cur_start += double_node_blks;
	}
	
	/* 处理最后一个triple node维护的block范围 */
	if (cur_start > end_blk)  return;
	node_block_cache_entry_handle handle = node_cache_helper(this->fs_manager)
		.get_node_entry(inode->i.i_nid[nid_idx], inode->footer.ino);
	if (proc_triple_node(handle, cur_start))
	{
		inode->i.i_nid[nid_idx] = INVALID_NID;
		handle.delete_node();
	}
	else
		handle.mark_dirty();
}

node_block_cache_entry_handle file_creator::create_generic_file()
{
	auto handle = create_base_inode();
	hscfs_inode *inode = &handle->get_node_block_ptr()->i;

	/* 对于普通文件，创建时文件大小为0，只需要再设置文件类型即可 */
	assert(inode->i_size == 0);
	inode->i_type = HSCFS_FT_REG_FILE;

	return handle;
}

node_block_cache_entry_handle file_creator::create_directory()
{
	auto handle = create_base_inode();
	hscfs_inode *inode = &handle->get_node_block_ptr()->i;

	/* 对于目录文件，需要建立第0级哈希表，调整文件大小，留下文件空洞即可 */
	inode->i_type = HSCFS_FT_DIR;
	assert(inode->i_dentry_num == 0);
	assert(inode->i_current_depth == 0);
	assert(inode->i_dir_level == 0);
	uint32_t blk_num = directory::bucket_num(inode->i_current_depth, inode->i_dir_level) * 
		directory::bucket_block_num(inode->i_current_depth);
	inode->i_size = blk_num * 4096;
	assert(inode->i_addr[0] == INVALID_LPA);
	assert(inode->i_addr[1] == INVALID_LPA);

	return handle;
}

node_block_cache_entry_handle file_creator::create_base_inode()
{
    node_cache_helper node_helper(fs_manager);
	auto handle = node_helper.create_inode_entry();
	hscfs_inode *inode = &handle->get_node_block_ptr()->i;
	
	/* 将inode置为刚刚创建的状态 */
	inode->i_nlink = 1;
	inode_time_util::set_atime(inode);
	inode_time_util::set_mtime(inode);
	/* 其余字段在缓存内存分配时自动初始化为0 */

	return handle;
}

void file_deletor::delete_file(uint32_t ino)
{
	/* 首先通过reduce，删除文件除inode外所有node block，并无效化所有数据块的lpa */
	file_resizer(fs_manager).reduce(ino, 0);

	/* 删除该文件的inode。上一步中一定会访问inode，所以这里直接通过缓存handle删除 */
	node_block_cache_entry_handle inode_handle = node_cache_helper(fs_manager).get_node_entry(ino, INVALID_NID);
	inode_handle.delete_node();
}

void file_deletor::delete_dir_with_data_cache(uint32_t ino)
{
	delete_file(ino);
	dir_data_block_cache *dir_data_cache = fs_manager->get_dir_data_cache();
	dir_data_cache->remove_ino_blks(ino);
}

void inode_time_util::set_atime(hscfs_inode *inode, const timespec *time)
{
	timespec cur_time;
	if (time == nullptr)
	{
		timespec_get(&cur_time, TIME_UTC);
		time = &cur_time;
	}
	inode->i_atime = time->tv_sec;
	inode->i_atime_nsec = time->tv_nsec;
}

void inode_time_util::set_mtime(hscfs_inode *inode, const timespec *time)
{
	timespec cur_time;
	if (time == nullptr)
	{
		timespec_get(&cur_time, TIME_UTC);
		time = &cur_time;
	}
	inode->i_mtime = time->tv_sec;
	inode->i_mtime_nsec = time->tv_nsec;
}

void inode_time_util::mark_access(uint32_t ino)
{
	auto inode_handle = node_cache_helper(fs_manager).get_node_entry(ino, INVALID_NID);
	hscfs_node *node = inode_handle->get_node_block_ptr();
	assert(node->footer.ino == ino);
	assert(node->footer.nid == ino);
	assert(node->footer.offset == 0);
	set_atime(&node->i);
	inode_handle.mark_dirty();
}

void inode_time_util::mark_modified(uint32_t ino)
{
	auto inode_handle = node_cache_helper(fs_manager).get_node_entry(ino, INVALID_NID);
	hscfs_node *node = inode_handle->get_node_block_ptr();
	assert(node->footer.ino == ino);
	assert(node->footer.nid == ino);
	assert(node->footer.offset == 0);
	set_atime(&node->i);
	set_mtime(&node->i);
	inode_handle.mark_dirty();
}

uint32_t hscfs::file_nlink_utils::sub_nlink(uint32_t ino)
{
    node_block_cache_entry_handle inode_handle = node_cache_helper(fs_manager).get_node_entry(ino, INVALID_NID);
	hscfs_inode *inode = &inode_handle->get_node_block_ptr()->i;
	assert(inode->i_nlink >= 1);
	--inode->i_nlink;
	inode_handle.mark_dirty();

	if (inode->i_type == HSCFS_FT_REG_FILE)
	{
		file_obj_cache *file_cache = fs_manager->get_file_obj_cache();
		if (file_cache->contains(ino))
		{
			file_handle file = file_cache->get(ino);
			assert(!file.is_empty());
			file->sub_nlink();  // 此处nlink与inode保持同步，不需要mark_dirty file
			assert(file->get_nlink() == inode->i_nlink);
		}
	}

	return inode->i_nlink;    
}

uint32_t file_nlink_utils::add_nlink(uint32_t ino)
{
    node_block_cache_entry_handle inode_handle = node_cache_helper(fs_manager).get_node_entry(ino, INVALID_NID);
	hscfs_inode *inode = &inode_handle->get_node_block_ptr()->i;
	++inode->i_nlink;
	inode_handle.mark_dirty();

	if (inode->i_type == HSCFS_FT_REG_FILE)
	{
		file_obj_cache *file_cache = fs_manager->get_file_obj_cache();
		if (file_cache->contains(ino))
		{
			file_handle file = file_cache->get(ino);
			assert(!file.is_empty());
			file->add_nlink();  // 此处nlink与inode保持同步，不需要mark_dirty file
			assert(file->get_nlink() == inode->i_nlink);
		}
	}

	return inode->i_nlink;  
}

} // namespace hscfs