#include "fs/NAT_utils.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "cache/super_cache.hh"
#include "cache/SIT_NAT_cache.hh"
#include "utils/hscfs_log.h"

#include <cassert>

namespace hscfs {

nat_lpa_mapping::nat_lpa_mapping(file_system_manager *fs_manager)
{
    nat_start_lpa = (*fs_manager->get_super_cache())->nat_blkaddr;
    nat_segment_cnt = (*fs_manager->get_super_cache())->segment_count_nat;
    this->fs_manager = fs_manager;
}

std::pair<uint32_t, uint32_t> nat_lpa_mapping::get_nid_pos_in_nat(uint32_t nid)
{
    uint32_t nat_lpa_idx = nid / NAT_ENTRY_PER_BLOCK;
    uint32_t nat_lpa_off = nid % NAT_ENTRY_PER_BLOCK;
    assert(nat_lpa_idx < nat_segment_cnt * BLOCK_PER_SEGMENT);
    return std::make_pair(nat_lpa_idx + nat_start_lpa, nat_lpa_off);
}

uint32_t nat_lpa_mapping::get_lpa_of_nid(uint32_t nid)
{
    auto pos = get_nid_pos_in_nat(nid);
    uint32_t nat_block_lpa = pos.first;
    uint32_t nat_entry_idx = pos.second;
    HSCFS_LOG(HSCFS_LOG_INFO, "nat entry pos of nid = %u: lpa = %u, idx in lpa = %u", 
        nid, nat_block_lpa, nat_entry_idx);
    SIT_NAT_cache_entry_handle nat_handle = fs_manager->get_nat_cache()->get(nat_block_lpa);
    hscfs_nat_entry nat_entry = nat_handle.get_nat_block_ptr()->entries[nat_entry_idx];
    uint32_t nid_lpa = nat_entry.block_addr;
    HSCFS_LOG(HSCFS_LOG_INFO, "lpa of nid: %u.", nid_lpa);
    return nid_lpa;
}

}