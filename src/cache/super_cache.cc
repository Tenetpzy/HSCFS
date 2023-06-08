#include "communication/comm_api.h"
#include "cache/super_cache.hh"
#include "utils/io_utils.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

void super_cache::read_super_block()
{
    try 
    {
        super_block.read_from_lpa(dev, sb_lpa);
    }
    catch (const std::exception &e)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "super cache: read super block error.");
        throw;
    }
}

}