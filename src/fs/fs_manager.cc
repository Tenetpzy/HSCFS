#include "cache/super_cache.hh"
#include "cache/dentry_cache.hh"
#include "cache/dir_data_block_cache.hh"
#include "cache/node_block_cache.hh"
#include "cache/SIT_NAT_cache.hh"
#include "fs/fd_array.hh"
#include "fs/file.hh"
#include "fs/srmap_utils.hh"
#include "fs/super_manager.hh"
#include "fs/fs_manager.hh"
#include "fs/replace_protect.hh"
#include "fs/server_thread.hh"
#include "fs/write_back_helper.hh"
#include "fs/replace_protect.hh"
#include "journal/journal_container.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/lock_guards.hh"
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

std::unique_ptr<file_system_manager> file_system_manager::g_fs_manager;

file_system_manager::~file_system_manager()
{
    int ret = rwlock_destroy(&fs_freeze_lock);
    if (ret != 0)
        HSCFS_LOG(HSCFS_LOG_WARNING, "file system manager: destruct fs freeze lock failed.");
}

void file_system_manager::init(comm_dev *device)
{
    g_fs_manager = std::make_unique<file_system_manager>();
    int ret = rwlock_init(&g_fs_manager->fs_freeze_lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "file system manager: init fs freeze lock failed.");

    g_fs_manager->super = std::make_unique<super_cache>(device, super_block_lpa);
    g_fs_manager->super->read_super_block();
    g_fs_manager->sp_manager = std::make_unique<super_manager>(g_fs_manager.get());
    g_fs_manager->d_cache = std::make_unique<dentry_cache>(dentry_cache_size, g_fs_manager.get());
    g_fs_manager->node_cache = std::make_unique<node_block_cache>(g_fs_manager.get(), node_cache_size);
    g_fs_manager->dir_data_cache = std::make_unique<dir_data_block_cache>(dir_data_cache_size);
    g_fs_manager->sit_cache = std::make_unique<SIT_NAT_cache>(device, sit_cache_size);
    g_fs_manager->nat_cache = std::make_unique<SIT_NAT_cache>(device, nat_cache_size);
    g_fs_manager->file_cache = std::make_unique<file_obj_cache>(file_cache_size, g_fs_manager.get());
    g_fs_manager->srmap_util = std::make_unique<srmap_utils>(g_fs_manager.get());

    g_fs_manager->dev = device;

    uint32_t root_inode = (*g_fs_manager->super)->root_ino;
    g_fs_manager->root_dentry = g_fs_manager->d_cache->add_root(root_inode);

    g_fs_manager->fd_arr = std::make_unique<fd_array>(fd_array_size);
    g_fs_manager->cur_journal = std::make_unique<journal_container>();
    g_fs_manager->rp_manager = std::make_unique<replace_protect_manager>(g_fs_manager.get());
    g_fs_manager->server_th = std::make_unique<server_thread>();
    g_fs_manager->server_th->start();

    g_fs_manager->is_unrecoverable = false;
}

void file_system_manager::fini()
{
    /* 首先获取fs_freeze_lock独占，此时只有调用线程能够操作文件系统层，后续无需再加下层其它锁 */
    {
        rwlock_guard fs_freeze_lg(g_fs_manager->fs_freeze_lock, rwlock_guard::lock_type::wrlock);

        /* 回写所有脏数据 */
        g_fs_manager->check_state();
        g_fs_manager->write_back_all_dirty_sync();

        /* 停止服务线程 */
        g_fs_manager->server_th->stop();
    }
    
    /* 析构fs_manager */
    g_fs_manager = nullptr;
    HSCFS_LOG(HSCFS_LOG_INFO, "destructed all file system cache.");
}

void do_close(int fd);

void file_system_manager::write_back_all_dirty_sync()
{
    /* 为了避免与淘汰保护任务死锁，内部不加fs_meta_lock锁，因为调用者已经加了fs_freeze_lock，此时文件系统层应只有调用者一个活跃线程 */

    /* 关闭所有文件描述符 */
    std::unordered_set<int> unclosed_fds = fd_arr->get_and_clear_unclosed_fds();
    for (int fd: unclosed_fds)
        do_close(fd);

    /* 首先回写所有的dirty files */
    std::unordered_map<uint32_t, file_handle> dirty_files = file_cache->get_and_clear_dirty_files();
    for (auto &entry: dirty_files)
    {
        file_handle &handle = entry.second;
        handle->write_back();
    }

    /* 然后回写所有的脏元数据 */
    write_back_helper wb_helper(this);
    wb_helper.write_meta_back_sync();

    /* 等待回写元数据的事务淘汰保护完成 */
    rp_manager->wait_all_protect_task_cplt();

    /* 
     * 刚刚回写元数据的事务中，可能包含未提交的segments，它们在淘汰保护任务完成后才会加入系统node/data segment链表
     * 所以，此时super block和SIT表仍然可能包含脏元数据，如果包含，则再次回写脏元数据
     */
    if (!cur_journal->is_empty())
    {
        wb_helper.write_meta_back_sync();
        rp_manager->wait_all_protect_task_cplt();
    }

    /* 此时所有脏数据和元数据应该都落盘了 */
    assert(cur_journal->is_empty());
}

std::unique_ptr<journal_container> file_system_manager::get_and_reset_cur_journal() noexcept
{
    std::unique_ptr<journal_container> ret = std::make_unique<journal_container>();
    cur_journal.swap(ret);
    return ret;
}

void file_system_manager::check_state() const
{
    if (is_unrecoverable)
        throw not_recoverable();
}

} // namespace hscfs