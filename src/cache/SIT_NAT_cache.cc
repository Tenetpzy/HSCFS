#include "communication/comm_api.h"
#include "utils/io_utils.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"
#include "cache/SIT_NAT_cache.hh"

#include <cassert>

namespace hscfs {

SIT_NAT_cache_entry::~SIT_NAT_cache_entry()
{
    if (ref_count > 0)
        HSCFS_LOG(HSCFS_LOG_WARNING, "SIT/NAT cache entry has non-zero refcount when destructed, refcount = %u,"
            " lpa = %u", ref_count, lpa_);
}

void SIT_NAT_cache::do_replace()
{
    if (cur_size > expect_size)
    {
        while (true)
        {
            auto p = cache_manager.replace_one();
            if (p != nullptr)
            {
                // 正确性检查，被置换出来的缓存项引用计数应该为0
                assert(p->ref_count == 0);
                --cur_size;
                HSCFS_LOG(HSCFS_LOG_INFO, "relpace SIT/NAT cache entry, lpa = %u", p->lpa_);
            }
            if (p == nullptr || cur_size <= expect_size)
                break;
        }
    }
}

void SIT_NAT_cache::read_lpa(SIT_NAT_cache_entry *entry)
{
    int ret = comm_submit_sync_rw_request(dev, entry->cache.get_ptr(), LPA_TO_LBA(entry->lpa_), 
        LBA_PER_LPA, COMM_IO_READ);
    if (ret != 0)
        throw io_error("SIT/NAT cache entry: read lpa failed.");
}

hscfs::SIT_NAT_cache_entry_handle::~SIT_NAT_cache_entry_handle()
{
    try 
    {
        cache_->put_cache_entry(entry_);
    }
    catch(const std::exception &e)
    {
        HSCFS_LOG(HSCFS_LOG_WARNING, "exception during sub_refcount of SIT_NAT_cache_entry: "
            "%s", e.what());
    }
}

void SIT_NAT_cache_entry_handle::add_refcount()
{
    cache_->add_refcount(entry_);
}

void SIT_NAT_cache_entry_handle::sub_refcount()
{
    cache_->sub_refcount(entry_);
}

void SIT_NAT_cache_entry_handle::add_host_version()
{
    cache_->add_host_version(entry_);
}

void SIT_NAT_cache_entry_handle::add_SSD_version()
{
    cache_->add_SSD_version(entry_);
}

} // namespace hscfs