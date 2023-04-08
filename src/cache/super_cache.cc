#include "communication/comm_api.h"
#include "cache/super_cache.hh"
#include "utils/io_utils.hh"
#include "utils/hscfs_exceptions.hh"

namespace hscfs {

void super_cache::read_super_block()
{
    int ret = comm_submit_sync_rw_request(dev, &super_block, LPA_TO_LBA(sb_lpa), LBA_PER_LPA, COMM_IO_READ);
    if (ret != 0)
        throw io_error("super cache: read super block error.");
}

}