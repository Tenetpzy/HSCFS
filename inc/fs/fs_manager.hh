#pragma once

#include <memory>
#include <mutex>
#include "cache/dentry_cache.hh"
#include "utils/hscfs_multithread.h"

struct comm_dev;

namespace hscfs {

class super_manager;
class super_cache;
class node_block_cache;
class SIT_NAT_cache;
class file_obj_cache;
class srmap_utils;
class fd_array;
class dir_data_block_cache;
class journal_container;

/*
 * super_manager, SIT cache, NAT cache等对象的组合容器
 */
class file_system_manager
{
public:
    file_system_manager() = default;

    /* g_fs_manager初始化，必须在通信层初始化之后进行 */
    static void init(comm_dev *dev);

    static file_system_manager* get_instance()
    {
        return &g_fs_manager;
    }

    std::mutex& get_fs_meta_lock() noexcept
    {
        return fs_meta_lock;
    }

    rwlock_t& get_fs_freeze_lock() noexcept
    {
        return fs_freeze_lock;
    }

    super_manager* get_super_manager() const noexcept
    {
        return sp_manager.get();
    }

    super_cache* get_super_cache() const noexcept
    {
        return super.get();
    }

    dentry_cache* get_dentry_cache() const noexcept
    {
        return d_cache.get();
    }

    node_block_cache* get_node_cache() const noexcept
    {
        return node_cache.get();
    }

    dir_data_block_cache* get_dir_data_cache() const noexcept
    {
        return dir_data_cache.get();
    }

    SIT_NAT_cache* get_nat_cache() const noexcept
    {
        return nat_cache.get();
    }

    SIT_NAT_cache* get_sit_cache() const noexcept
    {
        return sit_cache.get();
    }

    file_obj_cache* get_file_obj_cache() const noexcept
    {
        return file_cache.get();
    }

    srmap_utils* get_srmap_util() const noexcept
    {
        return srmap_util.get();
    }

    size_t get_page_cache_size() const noexcept
    {
        return 32;
    }

    comm_dev* get_device() const noexcept
    {
        return dev;
    }

    dentry_handle get_root_dentry() const noexcept
    {
        return root_dentry;
    }

    fd_array* get_fd_array() const noexcept
    {
        return fd_arr.get();
    }

    /* 获取运行时的修改日志容器，未提交时所有修改日志写入此容器 */
    journal_container* get_cur_journal() const noexcept
    {
        return cur_journal.get();
    }

    /* 返回cur_journal，然后将cur_journal清空 */
    std::unique_ptr<journal_container> get_and_reset_cur_journal() noexcept;

    /* 置为不可恢复状态 */
    void set_unrecoverable() noexcept
    {
        is_unrecoverable = true;
    }

    /* 若处于不可恢复状态，则抛出not_recoverable异常 */
    void check_state() const;

private:

    std::mutex fs_meta_lock;
    rwlock_t fs_freeze_lock;

    std::unique_ptr<super_cache> super;
    std::unique_ptr<super_manager> sp_manager;
    std::unique_ptr<dentry_cache> d_cache;
    std::unique_ptr<node_block_cache> node_cache;
    std::unique_ptr<dir_data_block_cache> dir_data_cache;
    std::unique_ptr<SIT_NAT_cache> sit_cache, nat_cache;
    std::unique_ptr<file_obj_cache> file_cache;
    std::unique_ptr<srmap_utils> srmap_util;

    comm_dev *dev;
    dentry_handle root_dentry;
    std::unique_ptr<fd_array> fd_arr;

    std::unique_ptr<journal_container> cur_journal;
    bool is_unrecoverable;

    static file_system_manager g_fs_manager;

    static uint64_t super_block_lpa;
    static size_t dentry_cache_size;
    static size_t node_cache_size;
    static size_t dir_data_cache_size;
    static size_t sit_cache_size;
    static size_t nat_cache_size;
    static size_t file_cache_size;
    static size_t fd_array_size;
};

}  // namespace hscfs