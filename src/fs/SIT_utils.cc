#include "fs/fs.h"
#include "fs/SIT_utils.hh"
#include "fs/fs_manager.hh"
#include "cache/super_cache.hh"
#include "cache/SIT_NAT_cache.hh"
#include "journal/journal_container.hh"
#include "utils/hscfs_log.h"
#include "SIT_utils.hh"

namespace hscfs {

SIT_operator::SIT_operator(file_system_manager *fs_manager)
{
    this->fs_manager = fs_manager;
    super_cache *super = fs_manager->get_super_cache();
    seg0_start_lpa = super->segment0_blkaddr;
    seg_count = super->segment_count;
    sit_start_lpa = super->sit_blkaddr;
    sit_segment_cnt = super->segment_count_sit;
}

void SIT_operator::change_lpa_state(uint32_t lpa, bool valid)
{
    /* 计算lpa的segment id，segment内块偏移，所处SIT表的lpa */
    lpa -= seg0_start_lpa;
    uint32_t segid = lpa / BLOCK_PER_SEGMENT;
    uint32_t segoff = lpa % BLOCK_PER_SEGMENT;
    uint32_t sit_lpa = sit_start_lpa + segid / SIT_ENTRY_PER_BLOCK;
    HSCFS_LOG(HSCFS_LOG_INFO, "lpa [%u]: segid = %u, segoff = %u, SIT lpa = %u", lpa, segid, segoff, sit_lpa);

    /* 计算segid在sit_block内对应的entry */
    SIT_NAT_cache *sit_cache = fs_manager->get_sit_cache();
    SIT_NAT_cache_entry_handle sit_block_handle = sit_cache->get(sit_lpa);
    hscfs_sit_block *sit_block = sit_block_handle.get_sit_block_ptr();
    hscfs_sit_entry &sit_entry = sit_block->entries[segid % SIT_ENTRY_PER_BLOCK];

    /* 修改对应sit entry */
    uint32_t bitmap_idx = segoff / 8;
    uint32_t bitmap_off = segoff % 8;

    if (valid)
    {
        assert(!(sit_entry.valid_map[bitmap_idx] & (1U << bitmap_off)));
        assert(GET_SIT_VBLOCKS(&sit_entry) <= BLOCK_PER_SEGMENT - 1);

        sit_entry.valid_map[bitmap_idx] |= 1U << bitmap_off;
        sit_entry.vblocks++;
    }

    else
    {
        assert(sit_entry.valid_map[bitmap_idx] & (1U << bitmap_off));
        assert(GET_SIT_VBLOCKS(&sit_entry) > 0);

        sit_entry.valid_map[bitmap_idx] &= ~(1U << bitmap_off);
        sit_entry.vblocks--;
    }

    /* 写下SIT日志条目，增加sit缓存块主机侧版本 */
    journal_container *cur_journal = fs_manager->get_cur_journal();
    SIT_journal_entry journal_entry = {.segID = segid, .newValue = sit_entry};
    cur_journal->append_SIT_journal_entry(journal_entry);
    sit_block_handle.add_host_version();
}

} // namespace hscfs