#pragma once

#include "utils/hscfs_multithread.h"

namespace hscfs {

class spin_lock_guard
{
public:
    spin_lock_guard(spinlock_t &lock);
    ~spin_lock_guard();

private:
    spinlock_t &lock_;
};

class rwlock_guard
{
public:
    enum class lock_type {
        rdlock, wrlock
    };

    rwlock_guard(rwlock_t &lock, lock_type type);
    ~rwlock_guard();

private:
    rwlock_t &lock_;
};

}  // namespace hscfs