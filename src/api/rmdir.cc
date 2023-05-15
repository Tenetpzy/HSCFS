#include "cache/dentry_cache.hh"
#include "cache/node_block_cache.hh"
#include "fs/fs_manager.hh"
#include "fs/path_utils.hh"
#include "fs/fs.h"
#include "fs/file_utils.hh"
#include "fs/directory.hh"
#include "utils/lock_guards.hh"
#include "utils/exception_handler.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

int rmdir(const char *pathname)
{
    file_system_manager *fs_manager = file_system_manager::get_instance();
    try 
    {
        rwlock_guard fs_freeze_lg(fs_manager->get_fs_freeze_lock(), rwlock_guard::lock_type::rdlock);
        std::lock_guard<std::mutex> fs_meta_lg(fs_manager->get_fs_meta_lock());
        fs_manager->check_state();

        try
        {
            std::string abs_path = path_helper::extract_abs_path(pathname);
            if (abs_path.length() == 0)
            {
                errno = EINVAL;
                return -1;
            }

            path_lookup_processor path_lookup_proc(fs_manager);
            path_lookup_proc.set_abs_path(abs_path);
            dentry_handle target_dentry = path_lookup_proc.do_path_lookup();

            if (!target_dentry.is_exist())
            {
                errno = ENOENT;
                return -1;
            }
            if (target_dentry->get_type() != HSCFS_FT_DIR)
            {
                errno = ENOTDIR;
                return -1;
            }

            /* 目标目录存在，检查目标是否为空目录 */
            node_block_cache_entry_handle inode_handle = node_cache_helper(fs_manager).get_node_entry(
                target_dentry->get_ino(), INVALID_NID);
            if (inode_handle->get_node_block_ptr()->i.i_dentry_num != 0)
            {
                errno = ENOTEMPTY;
                return -1;
            }

            HSCFS_LOG(HSCFS_LOG_DEBUG, "removing directory %s.", abs_path.c_str());
            uint32_t nlink = file_nlink_utils(fs_manager).sub_nlink(target_dentry->get_ino());
            assert(nlink == 0);

            /* 删除目录项 */
            auto &parent_key = target_dentry->get_key();
            auto parent_dentry = fs_manager->get_dentry_cache()->get(parent_key.dir_ino, parent_key.name);
            assert(!parent_dentry.is_empty());
            directory dir(parent_dentry, fs_manager);
            dir.remove(target_dentry);

            return 0;
        }
        catch(const std::exception& e)
        {
            errno = exception_handler(fs_manager, e).convert_to_errno();
            return -1;
        }
    }
    catch (const std::exception &e)
    {
        errno = exception_handler(fs_manager, e).convert_to_errno();
        return -1;
    }
}

}  // namespace hscfs


#ifdef CONFIG_C_API

extern "C" int rmdir(const char *pathname)
{
    return hscfs::rmdir(pathname);
}

#endif