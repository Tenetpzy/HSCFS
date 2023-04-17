#pragma once

#include <utility>
#include <cstdint>

struct hscfs_super_block;

namespace hscfs {

class NAT_operator
{
public:
    /* to do */
};

class nat_lpa_mapping
{
public:
    nat_lpa_mapping(uint32_t nat_start_lpa, uint32_t nat_segment_cnt)
    {
        this->nat_start_lpa = nat_start_lpa;
        this->nat_segment_cnt = nat_segment_cnt;
    }

    /* 返回nid在NAT表中的位置，在lpa的第idx项：<lpa，idx> */
    std::pair<uint32_t, uint32_t> get_nid_lpa_in_nat(uint32_t nid);

private:
    uint32_t nat_start_lpa;
    uint32_t nat_segment_cnt;

};

}