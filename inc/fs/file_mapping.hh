#pragma once

#include <cstdint>

struct hscfs_node;

namespace hscfs {

class file_system_manager;

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

    /* 得到文件ino中，块号blkno的lpa */
    uint32_t get_lpa_of_block(uint32_t ino, uint32_t blkno);

private:
    file_system_manager *fs_manager;

    static int get_node_path(uint64_t block, uint32_t offset[4], uint32_t noffset[4]);
    static uint32_t get_next_nid(hscfs_node *node, uint32_t offset, int cur_level);
    static uint32_t get_lpa(hscfs_node *node, uint32_t offset, int level);
};

}  // namespace hscfs