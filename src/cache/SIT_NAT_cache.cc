#include "communication/comm_api.h"
#include "utils/io_utils.hh"
#include "utils/hscfs_exceptions.hh"
#include "cache/SIT_NAT_cache.hh"

#include <cassert>

namespace hscfs {

void SIT_NAT_cache_entry::read_content(comm_dev *dev)
{
    int ret = comm_submit_sync_rw_request(dev, cache.get_ptr(), LPA_TO_LBA(lpa_), LBA_PER_LPA, COMM_IO_READ);
    if (ret != 0)
        throw io_error("SIT/NAT cache entry: read lpa failed.");
}

SIT_NAT_cache_entry *SIT_NAT_cache::get_cache_entry(uint32_t lpa)
{
    SIT_NAT_cache_entry *p = cache_manager.get(lpa);

    // 如果缓存不命中，则从SSD读取。
    // 若缓存数量超过阈值，则尝试置换。
    if (p == nullptr)
    {
        auto tmp = std::make_unique<SIT_NAT_cache_entry>(lpa);
        tmp->read_content(dev);
        p = tmp.get();

        ++cur_size;
        do_replace();

        cache_manager.add(lpa, tmp);
    }

    return p;
}

void SIT_NAT_cache::do_replace()
{
    if (cur_size >= expect_size)
    {
        while (true)
        {
            auto p = cache_manager.replace_one();
            if (p != nullptr)
            {
                // 正确性检查，被置换出来的缓存项引用计数应该为0
                assert(p->get_ref_count() == 0);
                --cur_size;
            }
            if (p == nullptr || cur_size < expect_size)
                break;
        }
    }
}

} // namespace hscfs