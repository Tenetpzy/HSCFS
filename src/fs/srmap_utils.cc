#include "cache/super_cache.hh"
#include "fs/srmap_utils.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "utils/hscfs_log.h"
#include "utils/io_utils.hh"
#include "utils/hscfs_exceptions.hh"
#include <tuple>

namespace hscfs {

srmap_utils::srmap_utils(file_system_manager *fs_manager)
{
    this->fs_manager = fs_manager;
    srmap_start_lpa = (*fs_manager->get_super_cache())->srmap_blkaddr;
}

srmap_utils::~srmap_utils()
{
    if (!dirty_blks.empty())
        HSCFS_LOG(HSCFS_LOG_WARNING, "SRMAP cache still has dirty lpa while destructed.");
}

void srmap_utils::write_srmap_of_data(uint32_t data_lpa, uint32_t nid, uint32_t offset)
{
    uint32_t srmap_lpa, srmap_idx;
    std::tie(srmap_lpa, srmap_idx) = get_srmap_pos_of_lpa(data_lpa);
    block_buffer &buffer = get_srmap_blk(srmap_lpa);
    hscfs_summary_block *srmap_blk = reinterpret_cast<hscfs_summary_block*>(buffer.get_ptr());
    srmap_blk->entries[srmap_idx].nid = nid;
    srmap_blk->entries[srmap_idx].ofs_in_node = offset;
    HSCFS_LOG(HSCFS_LOG_INFO, "set srmap of data lpa %u: nid = %u, offset = %u.", data_lpa, nid, offset);
    dirty_blks.insert(srmap_lpa);
}

void srmap_utils::write_srmap_of_node(uint32_t node_lpa, uint32_t nid)
{
    uint32_t srmap_lpa, srmap_idx;
    std::tie(srmap_lpa, srmap_idx) = get_srmap_pos_of_lpa(node_lpa);
    block_buffer &buffer = get_srmap_blk(srmap_lpa);
    hscfs_summary_block *srmap_blk = reinterpret_cast<hscfs_summary_block*>(buffer.get_ptr());
    srmap_blk->entries[srmap_idx].nid = nid;
    HSCFS_LOG(HSCFS_LOG_INFO, "set srmap of node lpa %u: nid = %u.", node_lpa, nid);
    dirty_blks.insert(srmap_lpa);
}

void srmap_utils::write_dirty_srmap_sync()
{
    async_vecio_synchronizer syn(dirty_blks.size());
    for (uint32_t lpa : dirty_blks)
    {
        srmap_cache[lpa].write_to_lpa_async(fs_manager->get_device(), lpa, 
            async_vecio_synchronizer::generic_callback, &syn);
    }
    comm_cmd_result res = syn.wait_cplt();
    if (res != comm_cmd_result::COMM_CMD_SUCCESS)
        throw io_error("write back srmap failed.");
}

void srmap_utils::clear_cache()
{
    dirty_blks.clear();
    srmap_cache.clear();
}

std::pair<uint32_t, uint32_t> srmap_utils::get_srmap_pos_of_lpa(uint32_t lpa)
{
    uint32_t idx = lpa / ENTRIES_IN_SUM;
    uint32_t off = lpa % ENTRIES_IN_SUM;
    HSCFS_LOG(HSCFS_LOG_DEBUG, "srmap pos of lpa %u: lpa = %u, idx in lpa = %u.", lpa, idx + srmap_start_lpa, off);
    return std::make_pair(idx + srmap_start_lpa, off);
}

block_buffer &srmap_utils::get_srmap_blk(uint32_t lpa)
{
    if (srmap_cache.count(lpa))
        return srmap_cache[lpa];
    srmap_cache[lpa].read_from_lpa(fs_manager->get_device(), lpa);
    return srmap_cache[lpa];
}

} // namespace hscfs