#pragma once

#include <system_error>
#include "utils/hscfs_log.h"
#include "utils/hscfs_multithread.h"

class spin_lock_guard
{
public:
    spin_lock_guard(spinlock_t *lock)
    {
        lock_ = lock;
        int ret = spin_lock(lock_);
        if (ret != 0)
            throw std::system_error(std::error_code(ret, std::generic_category()), "lock spin failed.");
    }

    ~spin_lock_guard()
    {
        int ret = spin_unlock(lock_);
        if (ret != 0)
            HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "unlock spin falied.");
    }

private:
    spinlock_t *lock_;
};