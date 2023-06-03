#include "cache/super_cache.hh"
#include "cache/dentry_cache.hh"
#include "cache/dir_data_block_cache.hh"
#include "cache/node_block_cache.hh"
#include "cache/SIT_NAT_cache.hh"
#include "fs/fd_array.hh"
#include "fs/file.hh"
#include "fs/srmap_utils.hh"
#include "fs/super_manager.hh"
#include "journal/journal_container.hh"
#include "fs/fs_manager.hh"
#include "utils/hscfs_exceptions.hh"
#include <system_error>

namespace hscfs {

uint64_t file_system_manager::super_block_lpa = 0;
size_t file_system_manager::dentry_cache_size = 128;
size_t file_system_manager::node_cache_size = 32;
size_t file_system_manager::dir_data_cache_size = 64;
size_t file_system_manager::sit_cache_size = 64;
size_t file_system_manager::nat_cache_size = 64;
size_t file_system_manager::file_cache_size = 32;
size_t file_system_manager::fd_array_size = 512;

file_system_manager file_system_manager::g_fs_manager;

void file_system_manager::init(comm_dev *device)
{
    int ret = rwlock_init(&g_fs_manager.fs_freeze_lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "file system manager: init fs freeze lock failed.");

    g_fs_manager.super = std::make_unique<super_cache>(device, super_block_lpa);
    g_fs_manager.super->read_super_block();
    g_fs_manager.sp_manager = std::make_unique<super_manager>(&g_fs_manager);
    g_fs_manager.d_cache = std::make_unique<dentry_cache>(dentry_cache_size, &g_fs_manager);
    g_fs_manager.node_cache = std::make_unique<node_block_cache>(&g_fs_manager, node_cache_size);
    g_fs_manager.dir_data_cache = std::make_unique<dir_data_block_cache>(dir_data_cache_size);
    g_fs_manager.sit_cache = std::make_unique<SIT_NAT_cache>(device, sit_cache_size);
    g_fs_manager.nat_cache = std::make_unique<SIT_NAT_cache>(device, nat_cache_size);
    g_fs_manager.file_cache = std::make_unique<file_obj_cache>(file_cache_size, &g_fs_manager);
    g_fs_manager.srmap_util = std::make_unique<srmap_utils>(&g_fs_manager);

    g_fs_manager.dev = device;

    uint32_t root_inode = (*g_fs_manager.super)->root_ino;
    g_fs_manager.root_dentry = g_fs_manager.d_cache->add_root(root_inode);

    g_fs_manager.fd_arr = std::make_unique<fd_array>(fd_array_size);
    g_fs_manager.cur_journal = std::make_unique<journal_container>();
    g_fs_manager.is_unrecoverable = false;
}

void file_system_manager::check_state() const
{
    if (is_unrecoverable)
        throw not_recoverable();
}

} // namespace hscfs