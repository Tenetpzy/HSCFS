#pragma once

#include <cstdint>

namespace hscfs {

class file_system_manager;

class SIT_operator
{
public:
    SIT_operator(file_system_manager *fs_manager);

    /* 将lpa无效化/有效化。修改对应SIT表项的有效块计数和位图，并写入修改日志 */
    void invalidate_lpa(uint32_t lpa)
    {
        change_lpa_state(lpa, false);
    }
    void validate_lpa(uint32_t lpa)
    {
        change_lpa_state(lpa, true);
    }

private:
    file_system_manager *fs_manager;
    uint32_t seg0_start_lpa;
    uint32_t seg_count;
    uint32_t sit_start_lpa;
    uint32_t sit_segment_cnt;

    void change_lpa_state(uint32_t lpa, bool valid);    
};

}