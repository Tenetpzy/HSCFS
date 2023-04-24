#pragma once

#include <utility>
#include <cstdint>

namespace hscfs {

class file_system_manager;

class nat_lpa_mapping
{
public:
    nat_lpa_mapping(file_system_manager *fs_manager);

    /* 返回nid在NAT表中的位置，在lpa的第idx项：<lpa，idx> */
    std::pair<uint32_t, uint32_t> get_nid_pos_in_nat(uint32_t nid);

    /* 访问NAT表，得到nid的lpa */
    uint32_t get_lpa_of_nid(uint32_t nid);

private:
    uint32_t nat_start_lpa;
    uint32_t nat_segment_cnt;
    file_system_manager *fs_manager;
};

}