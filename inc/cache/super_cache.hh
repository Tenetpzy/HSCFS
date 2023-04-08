#pragma once

#include <cstdint>
#include "fs/fs.h"

struct comm_dev;

namespace hscfs {

class super_cache
{
public:
    super_cache(comm_dev *device, uint64_t super_block_lpa) : sb_lpa(super_block_lpa) {
        dev = device;
    }

    void read_super_block();

    hscfs_super_block* operator->() {
        return &super_block;
    }

private:
    comm_dev *dev;
    const uint64_t sb_lpa;
    hscfs_super_block super_block;
};

}