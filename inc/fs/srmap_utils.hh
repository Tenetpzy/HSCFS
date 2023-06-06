#pragma once

#include <cstdint>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include "cache/block_buffer.hh"

namespace hscfs {

class file_system_manager;

/* SRMAP操作工具。使用此类需获取fs_meta_lock锁 */
class srmap_utils
{
public:
    srmap_utils(file_system_manager *fs_manager);
    ~srmap_utils();

    void write_srmap_of_data(uint32_t data_lpa, uint32_t ino, uint32_t blkoff);

    void write_srmap_of_node(uint32_t node_lpa, uint32_t nid);

    /* 将所有dirty的srmap block写回原位 */
    void write_dirty_srmap_sync();

    /* 清空srmap_cache */
    void clear_cache();

private:
    file_system_manager *fs_manager;
    uint32_t srmap_start_lpa;

    /* srmap的轻量级缓存。lpa映射到物理块缓存 */
    std::unordered_map<uint32_t, block_buffer> srmap_cache;
    std::unordered_set<uint32_t> dirty_blks;  // 记录srmap_cache中，哪些lpa是dirty的

    /* 返回lpa的反向映射在srmap中的<lpa, idx> */
    std::pair<uint32_t, uint32_t> get_srmap_pos_of_lpa(uint32_t lpa);

    /* 封装srmap_cache不命中时，从lpa读取SRMAP block到缓存的过程 */
    block_buffer& get_srmap_blk(uint32_t lpa);
};

}  // namespace hscfs