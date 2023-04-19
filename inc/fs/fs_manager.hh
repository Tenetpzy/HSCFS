#pragma once

#include <memory>
#include <mutex>
#include "cache/dentry_cache.hh"
#include "utils/hscfs_multithread.h"

struct comm_dev;

namespace hscfs {

class super_cache;
class node_block_cache;
class SIT_NAT_cache;
class file_obj_cache;
class fd_array;

/*
 * super_manager, SIT cache, NAT cache等对象的组合容器
 */
class file_system_manager
{
public:
    file_system_manager();

    static file_system_manager* get_instance();

    std::mutex& get_fs_meta_lock() noexcept
    {
        return fs_meta_lock;
    }

    rwlock_t& get_fs_freeze_lock() noexcept
    {
        return fs_freeze_lock;
    }

    super_cache* get_super_cache() const noexcept
    {
        return super.get();
    }
    dentry_cache* get_dentry_cache() const noexcept;
    node_block_cache* get_node_cache() const noexcept;
    SIT_NAT_cache* get_nat_cache() const noexcept;
    file_obj_cache* get_file_obj_cache() const noexcept;

    size_t get_page_cache_size() const noexcept
    {
        return 32;
    }

    comm_dev* get_device() const noexcept;
    dentry_handle get_root_dentry() const noexcept;
    fd_array* get_fd_array() const noexcept;

private:

    std::mutex fs_meta_lock;
    rwlock_t fs_freeze_lock;
    std::unique_ptr<super_cache> super;
};

}  // namespace hscfs