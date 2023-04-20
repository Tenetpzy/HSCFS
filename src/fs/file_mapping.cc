#include "fs/file_mapping.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "cache/node_block_cache.hh"
#include "communication/memory.h"
#include "communication/comm_api.h"
#include "utils/hscfs_log.h"

struct comm_dev;

namespace hscfs {

/* SSD file mapping查询控制器 */
class ssd_file_mapping_search_controller
{
public:
	ssd_file_mapping_search_controller(comm_dev *dev);
	

private:
	comm_dev *dev;
};


/* 
 * file mapping查询的辅助函数
 * block：带查找的块号
 * offset: block在索引树路径的每一级索引块中的偏移
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

uint32_t file_mapping_searcher::get_lpa_of_block(uint32_t ino, uint32_t blkno)
{
    uint32_t offset[4], noffset[4];
    int level = get_node_path(blkno, offset, noffset);
    assert(level != -1);
    node_block_cache *node_cache = fs_manager->get_node_cache();
    uint32_t cur_nid = ino;

    /* 依次读取搜索树路径上的每个node block，直到找到目标lpa */
    for (int i = 0; i <= level; ++i)
    {
        node_block_cache_entry_handle node_handle = node_cache->get(cur_nid);

        /* 当前node block缓存不命中，交给SSD进行查询 */
        if (node_handle.is_empty())
        {

        }
    }
}

} // namespace hscfs