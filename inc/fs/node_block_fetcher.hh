#pragma once

#include <cstdint>

#include "cache/node_block_cache.hh"

struct comm_dev;

namespace hscfs {

class SIT_NAT_cache;

/*
 * node block获取器
 * 封装node block cache不命中时，从NAT表中查找lpa并读入缓存的过程
 */
class node_block_fetcher
{
public:
    node_block_fetcher(comm_dev *device, SIT_NAT_cache *nat_cache, node_block_cache *node_cache, 
        uint32_t nat_start_lpa, uint32_t nat_segment_cnt) noexcept
    {
        dev = device;
        this->nat_cache = nat_cache;
        this->node_cache = node_cache;
        this->nat_start_lpa = nat_start_lpa;
        this->nat_segment_cnt = nat_segment_cnt;
    }

    node_block_cache_entry_handle get_node_entry(uint32_t nid, uint32_t parent_nid);

private:

    comm_dev *dev;
    SIT_NAT_cache *nat_cache;
    node_block_cache *node_cache;
    uint32_t nat_start_lpa;
    uint32_t nat_segment_cnt;
};

}  // namespace hscfs