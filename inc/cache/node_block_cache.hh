#pragma once

#include "cache/cache_manager.hh"

namespace hscfs {

class node_block_cache_entry
{
public:
    /* to do */

private:
    uint32_t nid;
    uint32_t ref_count;
    node_block_cache_entry *parent;
    uint32_t origin_lpa, commit_lpa;
    /* to do */
};

using node_block_cache = generic_cache_manager<uint32_t, node_block_cache_entry>;

}  // namespace hscfs