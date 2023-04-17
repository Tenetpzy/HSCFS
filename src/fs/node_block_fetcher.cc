#include "cache/SIT_NAT_cache.hh"
#include "fs/node_block_fetcher.hh"
#include "fs/NAT_utils.hh"
#include "utils/hscfs_log.h"
#include "utils/hscfs_exceptions.hh"

namespace hscfs {

node_block_cache_entry_handle node_block_fetcher::get_node_entry(uint32_t nid, uint32_t parent_nid)
{
    node_block_cache_entry_handle node_handle = node_cache->get(nid);
    if (node_handle.is_empty())
    {
        /* 从NAT表中得到nid block的lpa */
        auto pos = nat_lpa_mapping(nat_start_lpa, nat_segment_cnt).get_nid_lpa_in_nat(nid);
        uint32_t nat_block_lpa = pos.first;
        uint32_t nat_entry_idx = pos.second;
        HSCFS_LOG(HSCFS_LOG_INFO, "nat entry pos of nid = %u: lpa = %u, idx in lpa = %u", 
            nid, nat_block_lpa, nat_entry_idx);
        SIT_NAT_cache_entry_handle nat_handle = nat_cache->get(nat_block_lpa);
        hscfs_nat_entry nat_entry = nat_handle.get_nat_block_ptr()->entries[nat_entry_idx];
        uint32_t nid_lpa = nat_entry.block_addr;
        HSCFS_LOG(HSCFS_LOG_INFO, "lpa of nid: %u.", nid_lpa);

        block_buffer buf;
        try {
            buf.read_from_lpa(dev, nid_lpa);
        }
        catch (const io_error &e) {
            HSCFS_LOG(HSCFS_LOG_ERROR, "node block fetcher: read lpa %u failed.", nid_lpa);
            throw;
        }
        node_handle = node_cache->add(std::move(buf), nid, parent_nid, nid_lpa);
    }
    return node_handle;
}

}
