#pragma once

#include <cstdint>

namespace hscfs {

class file_system_manager;

class SIT_operator
{
public:
    SIT_operator(file_system_manager *fs_manager);

    /* 
     * 将lpa无效化/有效化。修改对应SIT表项的有效块计数和位图，并写入修改日志 
     * 如果lpa为INVALID_LPA，什么都不做。
     */
    void invalidate_lpa(uint32_t lpa)
    {
        change_lpa_state(lpa, false);
    }
    void validate_lpa(uint32_t lpa)
    {
        change_lpa_state(lpa, true);
    }

    /* 得到lpa所处的segment id和segment内块偏移<segid, off> */
    std::pair<uint32_t, uint32_t> get_seg_pos_of_lpa(uint32_t lpa);

    /* 返回segid在SIT表中的位置，在lpa的第idx项：<lpa, idx> */
    std::pair<uint32_t, uint32_t> get_segid_pos_in_sit(uint32_t segid);

    /* 得到segid对应segment的第一个block的lpa */
    uint32_t get_first_lpa_of_segid(uint32_t segid);

private:
    file_system_manager *fs_manager;
    uint32_t seg0_start_lpa;
    uint32_t seg_count;
    uint32_t sit_start_lpa;
    uint32_t sit_segment_cnt;

    void change_lpa_state(uint32_t lpa, bool valid);    
};

}