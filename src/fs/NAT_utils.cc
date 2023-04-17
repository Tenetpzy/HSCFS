#include "fs/NAT_utils.hh"
#include "fs/fs.h"

#include <cassert>

namespace hscfs {

std::pair<uint32_t, uint32_t> nat_lpa_mapping::get_nid_lpa_in_nat(uint32_t nid)
{
    uint32_t nat_lpa_idx = nid / NAT_ENTRY_PER_BLOCK;
    uint32_t nat_lpa_off = nid % NAT_ENTRY_PER_BLOCK;
    assert(nat_lpa_idx < nat_start_lpa + nat_segment_cnt * BLOCK_PER_SEGMENT);
    return std::make_pair(nat_lpa_idx + nat_start_lpa, nat_lpa_off);
}

}