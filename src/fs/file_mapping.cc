#include "fs/file_mapping.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "fs/NAT_utils.hh"
#include "cache/node_block_cache.hh"
#include "communication/memory.h"
#include "communication/comm_api.h"
#include "utils/hscfs_log.h"
#include "utils/hscfs_exceptions.hh"
#include "utils/dma_buffer_deletor.hh"

#include <memory>

struct comm_dev;

namespace hscfs {

/* SSD file mapping查询控制器 */
class ssd_file_mapping_search_controller
{
public:
	ssd_file_mapping_search_controller(comm_dev *device)
	{
		dev = device;
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

/* 
 * file mapping查询的辅助函数

 * block：带查找的块号
 * 
 * offset: block在索引树路径的每一级索引块中的偏移：
 * 对于inode，offset是i_attr数组下标
 * 对于indirect和direct node，offset是nid/addr数组下标
 * 
 * noffset：索引树路径的每一个块的树上逻辑编号
 * 
 * 返回值：block的索引树路径深度。inode深度为0。
 */
int file_mapping_searcher::get_node_path(uint64_t block, uint32_t offset[4], uint32_t noffset[4])
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

uint32_t file_mapping_searcher::get_next_nid(hscfs_node *node, uint32_t offset, int cur_level)
{
    if (cur_level == 0)  // 如果是inode
		return node->i.i_nid[offset - NODE_DIR1_BLOCK];
	return node->in.nid[offset];
}

uint32_t file_mapping_searcher::get_lpa(hscfs_node *node, uint32_t offset, int level)
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

block_addr_info file_mapping_searcher::get_addr_of_block(uint32_t ino, uint32_t blkno)
{
    uint32_t offset[4], noffset[4];
    int level = get_node_path(blkno, offset, noffset);
    assert(level != -1);
	HSCFS_LOG(HSCFS_LOG_INFO, "file mapping searcher: start file mapping search: "
		"target inode: %u, target block offset: %u, target path level in node tree: %d",
		ino, blkno, level
	);

    node_block_cache *node_cache = fs_manager->get_node_cache();

	hscfs_node *last_node = nullptr;  // 拥有指向blkno的direct point的node
	uint32_t cur_nid = ino;  // 当前查找的nid
	uint32_t parent_nid = INVALID_NID;  // cur_nid的父nid
	int cur_level = 0;  // 当前查找的路径层级(inode为0，子结点递增)

    while (true)
    {
        node_block_cache_entry_handle cur_handle = node_cache->get(cur_nid);

        /* 当前node block缓存不命中，交给SSD进行查询 */
        if (cur_handle.is_empty())
        {
			HSCFS_LOG(HSCFS_LOG_INFO, "file_mapping_searcher:"
				"node block[file(inode: %u), level %d, nid %u] miss. Prepare fetching from SSD", 
				ino, cur_level, cur_nid);
			int ssd_level = level - cur_level + 1;
			ssd_file_mapping_search_controller ctrlr(fs_manager->get_device());
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
	return ret;
}

} // namespace hscfs