#include "cache/dentry_cache.hh"
#include "fs/fs_manager.hh"
#include "fs/path_utils.hh"
#include "fs/fs.h"
#include "fs/file.hh"
#include "fs/file_utils.hh"
#include "fs/directory.hh"
#include "utils/lock_guards.hh"
#include "utils/exception_handler.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

/*
 * 移除硬链接pathname
 *
 * 若出错，返回-1，置errno为：
 * ENOENT：pathname不存在，或为空
 * EISDIR: pathname是目录，unlink不能用来删除目录
 */
int unlink(const char *pathname)
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
                errno = ENOENT;
                return -1;
            }

            path_lookup_processor path_lookup_proc(fs_manager);
            path_lookup_proc.set_abs_path(abs_path);
            dentry_handle target_dentry = path_lookup_proc.do_path_lookup();

            if (target_dentry.is_empty() || target_dentry->get_state() != dentry_state::valid)
            {
                errno = ENOENT;
                return -1;
            }

            if (target_dentry->get_type() == HSCFS_FT_DIR)
            {
                errno = EISDIR;
                return -1;
            }

            /* 减少文件的硬链接数。如果硬链接数减小至0，删除该文件 */
            uint32_t nlink = file_deletor(fs_manager).sub_nlink(target_dentry->get_ino());
            HSCFS_LOG(HSCFS_LOG_INFO, "unlink target file(name = %s, ino = %u)'s nlink equals to %u now.", 
                target_dentry->get_key().name.c_str(), target_dentry->get_ino(), nlink);

            if (nlink == 0)
            {
                /* 如果目标文件仍有fd引用，等待全部close后删除 */
                if (target_dentry->get_fd_refcount() > 0)
                {
                    HSCFS_LOG(HSCFS_LOG_INFO, "unlink target file(name = %s, ino = %u) is still referred by fd, "
                        "will be deleted later.", target_dentry->get_key().name.c_str(), target_dentry->get_ino());
                    target_dentry->set_state(dentry_state::deleted_referred_by_fd);
                }
                else  // 否则直接删除
                {
                    HSCFS_LOG(HSCFS_LOG_INFO, "delete unlink target file(name = %s, ino = %u).", 
                        target_dentry->get_key().name.c_str(), target_dentry->get_ino());

                    target_dentry->set_state(dentry_state::deleted);
                    uint32_t target_ino = target_dentry->get_ino();
                    file_obj_cache *file_cache = fs_manager->get_file_obj_cache();

                    /* 如果file对象缓存中存在file对象，则通过file handle删除(可同时删除file对象) */
                    if (file_cache->contains(target_ino))
                    {
                        file_handle target_handle = file_cache_helper(file_cache).get_file_obj(target_ino, 
                            target_dentry);
                        target_handle.delete_file();
                    }

                    /* 没有file对象，直接在文件系统中删除 */
                    else
                        file_deletor(fs_manager).delete_file(target_ino);
                }

                target_dentry.mark_dirty();

                /* 在父目录文件中删除目录项 */
                auto &parent_key = target_dentry->get_parent_key();
                auto parent_dentry = fs_manager->get_dentry_cache()->get(parent_key.dir_ino, parent_key.name);
                assert(!parent_dentry.is_empty());
                directory parent_dir(parent_dentry, fs_manager);
                parent_dir.remove(target_dentry);
            }
            
            return 0;
        }
        catch(const std::exception &e)
        {
            errno = exception_handler(fs_manager, e).convert_to_errno(true);
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

extern "C" int unlink(const char *pathname)
{
    return hscfs::unlink(pathname);
}

#endif