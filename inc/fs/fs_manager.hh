#pragma once

#include <memory>
#include <mutex>
#include "cache/dentry_cache.hh"

namespace hscfs {

class super_cache;

/*
 * super_manager, SIT cache, NAT cache等对象的组合容器
 */
class file_system_manager
{
public:
    static file_system_manager* get_instance();

    std::mutex& get_fs_lock_ref() noexcept
    {
        return fs_lock;
    }

    super_cache* get_super_cache() const noexcept
    {
        return super.get();
    }

    dentry_handle get_root_dentry() const noexcept;

    dentry_cache* get_dentry_cache() const noexcept;

private:

    std::mutex fs_lock;
    std::unique_ptr<super_cache> super;
};

}  // namespace hscfs