#include "utils/lock_guards.hh"
#include "utils/hscfs_log.h"
#include <system_error>

namespace hscfs {

spin_lock_guard::spin_lock_guard(spinlock_t &lock)
    : lock_(lock)
{
    int ret = spin_lock(&lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "spin lock guard: lock spin failed.");
}

spin_lock_guard::~spin_lock_guard()
{
    int ret = spin_unlock(&lock_);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "spin lock guard: unlock spin falied.");
}

rwlock_guard::rwlock_guard(rwlock_t &lock, lock_type type)
    : lock_(lock)
{
    int ret = 0;
    if (type == lock_type::rdlock)
        ret = rwlock_rdlock(&lock_);
    else
        ret = rwlock_wrlock(&lock_);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "rwlock guard: lock failed.");
}

rwlock_guard::~rwlock_guard()
{
    int ret = rwlock_unlock(&lock_);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "rwlock guard: unlock falied.");
}

}  // namespace hscfs