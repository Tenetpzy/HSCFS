#include "fs/fs.h"
#include "fs/super_manager.hh"
#include "fs/fs_manager.hh"
#include "fs/NAT_utils.hh"
#include "cache/super_cache.hh"
#include "cache/SIT_NAT_cache.hh"
#include "journal/journal_container.hh"
#include "utils/hscfs_log.h"

#include <tuple>
#include <cstddef>

namespace hscfs {

super_manager::super_manager(file_system_manager *fs_manager)
{
    this->fs_manager = fs_manager;
    super = fs_manager->get_super_cache();
}

void super_manager::free_nid(uint32_t nid)
{
    uint32_t nat_blk_lpa, nat_blk_idx;
    std::tie(nat_blk_lpa, nat_blk_idx) = nat_lpa_mapping(fs_manager).get_nid_pos_in_nat(nid);
    HSCFS_LOG(HSCFS_LOG_INFO, "nat entry pos of nid = %u: lpa = %u, idx in lpa = %u", 
        nid, nat_blk_lpa, nat_blk_idx);
    SIT_NAT_cache_entry_handle nat_handle = fs_manager->get_nat_cache()->get(nat_blk_lpa);
    hscfs_nat_entry &nat_entry = nat_handle.get_nat_block_ptr()->entries[nat_blk_idx];
    
    /* 将nid插入空闲nid链表 */
    HSCFS_LOG(HSCFS_LOG_INFO, "free nid [%u]. (the original free nid head is [%u]).", nid, super->next_free_nid);
    nat_entry.ino = 0;
    nat_entry.block_addr = super->next_free_nid;
    super->next_free_nid = nid;

    /* 记录修改的日志 */
    journal_container *cur_journal = fs_manager->get_cur_journal();
    NAT_journal_entry nat_journal = {.nid = nid, .newValue = nat_entry};
    super_block_journal_entry super_journal = {.Off = offsetof(hscfs_super_block, next_free_nid), .newVal = nid};
    cur_journal->append_NAT_journal_entry(nat_journal);
    cur_journal->append_super_block_journal_entry(super_journal);
    nat_handle.add_host_version();
}

} // namespace hscfs