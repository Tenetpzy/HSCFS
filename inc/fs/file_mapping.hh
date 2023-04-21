#pragma once

#include <cstdint>

struct hscfs_node;

namespace hscfs {

class file_system_manager;

/*
 * 保存一个block的地址信息
 * 包含它的物理地址lpa，和反向映射
 * 
 * 反向映射中的nid_off：
 * 若是inode中的direct_pointer，则nid_off是i_addr数组下标
 * 否则是direct node，nid_off是addr数组下标
 */
struct block_addr_info
{
    uint32_t lpa;
    uint32_t nid;
    uint32_t nid_off;
};

/*
 * file mapping解析的执行器
 * 使用此类前需要持有fs_meta_lock
 */
class file_mapping_searcher
{
public:
    file_mapping_searcher(file_system_manager *fs_manager)
    {
        this->fs_manager = fs_manager;
    }

    /* 
     * 得到文件ino中，块号blkno的地址信息
     * 调用者需保证ino和blkno合法 
     */
    block_addr_info get_addr_of_block(uint32_t ino, uint32_t blkno);

private:
    file_system_manager *fs_manager;

    static int get_node_path(uint64_t block, uint32_t offset[4], uint32_t noffset[4]);
    static uint32_t get_next_nid(hscfs_node *node, uint32_t offset, int cur_level);
    static uint32_t get_lpa(hscfs_node *node, uint32_t offset, int level);
};

}  // namespace hscfs