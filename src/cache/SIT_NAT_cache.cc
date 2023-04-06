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

/* 拷贝构造handle，将增加entry的引用计数 */
SIT_NAT_cache_entry_handle::SIT_NAT_cache_entry_handle(const SIT_NAT_cache_entry_handle &o)
    : cache_(o.cache_)
{
    entry_ = o.entry_;
    do_addref();
}

/* 赋值函数，减少之前entry的引用计数，增加赋值目标的引用计数 */
SIT_NAT_cache_entry_handle &SIT_NAT_cache_entry_handle::operator=(const SIT_NAT_cache_entry_handle &o)
{
    if (this != &o)
    {
        do_subref();
        entry_ = o.entry_;
        cache_ = o.cache_;
        do_addref();
    }
    return *this;
}

/* 移动赋值，减少之前entry的引用计数，接管赋值目标 */
SIT_NAT_cache_entry_handle &SIT_NAT_cache_entry_handle::operator=(SIT_NAT_cache_entry_handle &&o) noexcept
{
    if (this != &o)
    {
        do_subref();
        entry_ = o.entry_;
        cache_ = std::move(o.cache_);
    }
    return *this;
}

hscfs::SIT_NAT_cache_entry_handle::~SIT_NAT_cache_entry_handle()
{
    if (entry_ != nullptr)
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
}

/* SIT_NAT_cache_entry_handle和SIT_NAT_cache循环依赖，故以下成员无法在类中定义 */

void SIT_NAT_cache_entry_handle::add_host_version()
{
    cache_->add_host_version(entry_);
}

void SIT_NAT_cache_entry_handle::add_SSD_version()
{
    cache_->add_SSD_version(entry_);
}

void SIT_NAT_cache_entry_handle::do_addref()
{
    if (entry_ != nullptr)
        cache_->add_refcount(entry_);
}

void SIT_NAT_cache_entry_handle::do_subref()
{
    if (entry_ != nullptr)
        cache_->sub_refcount(entry_);
}

} // namespace hscfs