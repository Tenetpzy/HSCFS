#include "fs/fs.h"
#include "fs/super_manager.hh"
#include "fs/fs_manager.hh"
#include "fs/NAT_utils.hh"
#include "fs/SIT_utils.hh"
#include "cache/super_cache.hh"
#include "cache/SIT_NAT_cache.hh"
#include "journal/journal_container.hh"
#include "utils/hscfs_log.h"
#include "utils/hscfs_exceptions.hh"

#include <tuple>
#include <cstddef>

namespace hscfs {

super_manager::super_manager(file_system_manager *fs_manager)
    : super(*fs_manager->get_super_cache())
{
    this->fs_manager = fs_manager;
}

uint32_t super_manager::alloc_nid(uint32_t ino, bool is_inode)
{
    uint32_t nid = super->next_free_nid;
    if (nid == INVALID_NID)
        throw no_free_nid();

    uint32_t nat_blk_lpa, nat_blk_idx;
    std::tie(nat_blk_lpa, nat_blk_idx) = nat_lpa_mapping(fs_manager).get_nid_pos_in_nat(nid);
    SIT_NAT_cache_entry_handle nat_handle = fs_manager->get_nat_cache()->get(nat_blk_lpa);
    hscfs_nat_entry &nat_entry = nat_handle.get_nat_block_ptr()->entries[nat_blk_idx];

    /* 将空闲链表置为下一项，初始化nid对应的NAT表项 */
    assert(nat_entry.ino == 0);
    uint32_t nxt_nid = nat_entry.block_addr;
    HSCFS_LOG(HSCFS_LOG_INFO, "alloc nid [%u]. The next free nid is [%u].", nid, nxt_nid);
    super->next_free_nid = nxt_nid;
    
    if (is_inode)
        nat_entry.ino = nid;
    else
        nat_entry.ino = ino;
    nat_entry.block_addr = INVALID_LPA;

    /* 记录修改的日志 */
    journal_container *cur_journal = fs_manager->get_cur_journal();
    NAT_journal_entry nat_journal = {.nid = nid, .newValue = nat_entry};
    super_block_journal_entry super_journal = {.Off = offsetof(hscfs_super_block, next_free_nid), .newVal = nxt_nid};
    cur_journal->append_NAT_journal_entry(nat_journal);
    cur_journal->append_super_block_journal_entry(super_journal);
    nat_handle.add_host_version();

    return nid;
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
    HSCFS_LOG(HSCFS_LOG_INFO, "free nid [%u]. The original free nid head is [%u].", nid, super->next_free_nid);
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

uint32_t super_manager::alloc_node_lpa()
{
    lpa_alloc_context ctx = create_lpa_alloc_context(lpa_alloc_type::node);
    return alloc_lpa_inner(ctx);
}

uint32_t super_manager::alloc_data_lpa()
{
    lpa_alloc_context ctx = create_lpa_alloc_context(lpa_alloc_type::data);
    return alloc_lpa_inner(ctx);
}

void super_manager::add_to_node_segment_list(uint32_t segid)
{
    add_seg_to_list(segid, super->first_node_segment_id, offsetof(hscfs_super_block, first_node_segment_id));
}

void super_manager::add_to_data_segment_list(uint32_t segid)
{
    add_seg_to_list(segid, super->first_data_segment_id, offsetof(hscfs_super_block, first_data_segment_id));
}

std::vector<uint32_t> super_manager::get_and_clear_uncommit_node_segs()
{
    std::vector<uint32_t> ret;
    uncommit_node_segs.swap(ret);
    assert(uncommit_node_segs.size() == 0);
    return ret;
}

std::vector<uint32_t> super_manager::get_and_clear_uncommit_data_segs()
{
    std::vector<uint32_t> ret;
    uncommit_data_segs.swap(ret);
    assert(uncommit_data_segs.size() == 0);
    return ret;
}

lpa_alloc_context super_manager::create_lpa_alloc_context(lpa_alloc_type type)
{
    if (type == lpa_alloc_type::node)
    {
        lpa_alloc_context ctx(super->current_node_segment_id, offsetof(hscfs_super_block, current_node_segment_id), 
            super->current_node_segment_blkoff, offsetof(hscfs_super_block, current_node_segment_blkoff), 
            uncommit_node_segs);
        return ctx;
    }
    else
    {
        lpa_alloc_context ctx(super->current_data_segment_id, offsetof(hscfs_super_block, current_data_segment_id), 
            super->current_data_segment_blkoff, offsetof(hscfs_super_block, current_data_segment_blkoff), 
            uncommit_data_segs);
        return ctx;
    }
}

uint32_t super_manager::alloc_segment()
{
    /* 找到下一个空闲的segid */
    if (super->free_segment_count == 0)
        throw no_free_segment();
    uint32_t segid = super->first_free_segment_id;

    /* 从segid在sit表中的记录获取空闲链表的下一项 */
    uint32_t sit_lpa, sit_off;
    std::tie(sit_lpa, sit_off) = SIT_operator(fs_manager).get_segid_pos_in_sit(segid);
    SIT_NAT_cache_entry_handle sit_handle = fs_manager->get_sit_cache()->get(sit_lpa);
    hscfs_sit_entry &sit_entry = sit_handle.get_sit_block_ptr()->entries[sit_off];

    uint32_t nxt_segid = GET_NEXT_SEG(&sit_entry);
    assert(GET_SIT_VBLOCKS(&sit_entry) == 0);
    HSCFS_LOG(HSCFS_LOG_INFO, "alloc segment id [%u], the next free segment id is [%u].", segid, nxt_segid);

    /* 将空闲链表置为下一项 */
    super->first_free_segment_id = nxt_segid;
    --super->free_segment_count;

    /* 记录修改日志 */
    journal_container *cur_journal = fs_manager->get_cur_journal();
    super_block_journal_entry super_journal = {.Off = offsetof(hscfs_super_block, first_free_segment_id), 
        .newVal = nxt_segid};
    cur_journal->append_super_block_journal_entry(super_journal);
    super_journal.Off = offsetof(hscfs_super_block, free_segment_count);
    super_journal.newVal = super->free_segment_count;
    cur_journal->append_super_block_journal_entry(super_journal);

    return segid;
}

void super_manager::add_seg_to_list(uint32_t segid, uint32_t &list_head_segid, uint32_t list_head_addr_off)
{
    uint32_t sit_lpa, sit_off;
    std::tie(sit_lpa, sit_off) = SIT_operator(fs_manager).get_segid_pos_in_sit(segid);
    SIT_NAT_cache_entry_handle sit_handle = fs_manager->get_sit_cache()->get(sit_lpa);
    hscfs_sit_entry &sit_entry = sit_handle.get_sit_block_ptr()->entries[sit_off];

    SET_NEXT_SEG(&sit_entry, list_head_segid);  /* SIT表中指向当前链表头 */
    list_head_segid = segid;  /* super block中链表头指向segid */

    /* 记录修改的日志 */
    journal_container *cur_journal = fs_manager->get_cur_journal();
    super_block_journal_entry super_journal = {.Off = list_head_addr_off, .newVal = list_head_segid};
    cur_journal->append_super_block_journal_entry(super_journal);
    SIT_journal_entry sit_journal = {.segID = segid, .newValue = sit_entry};
    cur_journal->append_SIT_journal_entry(sit_journal);
}

uint32_t super_manager::alloc_lpa_inner(lpa_alloc_context &ctx)
{
    journal_container *cur_journal = fs_manager->get_cur_journal();

    /* 如果当前segment已经分配完了，则把它加入到未提交列表，并分配一个新的segment作为活跃segment */
    if (ctx.cur_seg_off == BLOCK_PER_SEGMENT)
    {
        HSCFS_LOG(HSCFS_LOG_INFO, "segment [%u] is fully written, add to uncommit list.", ctx.cur_seg_id);
        ctx.uncommit_segs.emplace_back(ctx.cur_seg_id);

        uint32_t new_segid = alloc_segment();
        ctx.cur_seg_id = new_segid;
        ctx.cur_seg_off = 0;

        /* 记录活跃segment id的修改日志, segoff的日志分配lpa后记录 */
        super_block_journal_entry super_journal = {.Off = ctx.seg_id_addr_offset, .newVal = new_segid};
        cur_journal->append_super_block_journal_entry(super_journal);
    }

    /* 计算分配出去的lpa */
    SIT_operator sit_operator(fs_manager);
    uint32_t lpa = sit_operator.get_first_lpa_of_segid(ctx.cur_seg_id) + ctx.cur_seg_off;
    HSCFS_LOG(HSCFS_LOG_INFO, "alloc lpa [%u] in segment [%u], segment block offset [%u].", lpa, ctx.cur_seg_id, 
        ctx.cur_seg_off);
    ++ctx.cur_seg_off;

    /* 记录活跃segment分配位置的修改日志 */
    super_block_journal_entry super_journal = {.Off = ctx.seg_off_addr_offset, .newVal = ctx.cur_seg_off};
    cur_journal->append_super_block_journal_entry(super_journal);

    /* 在SIT表中标记该块有效 */
    sit_operator.validate_lpa(lpa);

    return lpa;
}

} // namespace hscfs