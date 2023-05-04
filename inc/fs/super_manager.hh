#pragma once

#include <cstdint>
#include <vector>

namespace hscfs {

class file_system_manager;
class super_cache;

struct lpa_alloc_context
{
    uint32_t &cur_seg_id;  // 指向current_data_segment_id或current_node_segment_id
    uint32_t seg_id_addr_offset;  // cur_seg_id在hscfs_super_block中的偏移量，记录日志用
    uint32_t &cur_seg_off;  // 指向current_data_segment_blkoff或current_node_segment_blkoff
    uint32_t seg_off_addr_offset;  // cur_seg_off在hscfs_super_block中的偏移量，记录日志用
    std::vector<uint32_t> &uncommit_segs;  // 指向uncommit_node_segs或uncommit_data_segs

    lpa_alloc_context(uint32_t &cur_seg_id_, uint32_t seg_id_offset, uint32_t &cur_seg_off_, 
        uint32_t seg_off_offset, std::vector<uint32_t> &uncommit_segs_)
        : cur_seg_id(cur_seg_id_), cur_seg_off(cur_seg_off_), uncommit_segs(uncommit_segs_) 
    {
        this->seg_id_addr_offset = seg_id_offset;
        this->seg_off_addr_offset = seg_off_offset;
    }
};

/* 负责超级块中的资源分配、释放和维护 */
class super_manager
{
public:
    super_manager(file_system_manager *fs_manager);

    /* 分配nid，记录修改日志。ino是待分配的nid所属的文件inode。返回分配的nid */
    uint32_t alloc_nid(uint32_t ino);

    /* 释放nid，把它加入空闲nid链表，并将修改记录到super和NAT日志 */
    void free_nid(uint32_t nid);

    /* 
     * 为node/data block分配一个lpa，在对应的活跃segment上进行分配，
     * 如果活跃segment已满，将其加入对应uncommit_segs，从free segs上分配一个作为新的活跃segment，
     * 返回分配的lpa 
     */
    uint32_t alloc_node_lpa();
    uint32_t alloc_data_lpa();

private:
    file_system_manager *fs_manager;
    super_cache &super;
    std::vector<uint32_t> uncommit_node_segs, uncommit_data_segs;

private:

    enum class lpa_alloc_type {
        node, data
    };

    lpa_alloc_context create_lpa_alloc_context(lpa_alloc_type type);

    /* 从空闲segment链表中分配一个segment，记录修改日志，返回segment id */
    uint32_t alloc_segment();

    uint32_t alloc_lpa_inner(lpa_alloc_context &ctx);
};

}