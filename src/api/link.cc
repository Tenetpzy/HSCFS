#include "fs/fs_manager.hh"
#include "fs/path_utils.hh"
#include "fs/fs.h"
#include "fs/file_utils.hh"
#include "fs/directory.hh"
#include "utils/lock_guards.hh"
#include "utils/exception_handler.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

/*
 * 创建硬链接newpath，链接到oldpath
 *
 * 若出错，返回-1，置errno为：
 * ENOENT：newpath/oldpath不存在
 * EISDIR: oldpath为目录，不允许创建到目录的硬链接
 * EEXIST: newpath已经存在
 * ENOTRECOVERABLE：文件系统出现内部错误，无法恢复正常状态
 */
int link(const char *oldpath, const char *newpath)
{
    file_system_manager *fs_manager = file_system_manager::get_instance();
    try 
    {
        rwlock_guard fs_freeze_lg(fs_manager->get_fs_freeze_lock(), rwlock_guard::lock_type::rdlock);
        std::lock_guard<std::mutex> fs_meta_lg(fs_manager->get_fs_meta_lock());
        fs_manager->check_state();

        try
        {
            std::string old_path = path_helper::extract_abs_path(oldpath);
            std::string new_path = path_helper::extract_abs_path(newpath);

            if (old_path.length() == 0 || new_path.length() == 0)
            {
                errno = EINVAL;
                return -1;
            }

            path_lookup_processor path_lookup_proc(fs_manager);

            /* 检查old_path：存在且不能是目录 */
            path_lookup_proc.set_abs_path(old_path);
            dentry_handle old_dentry = path_lookup_proc.do_path_lookup();
            if (!old_dentry.is_exist())
            {
                errno = ENOENT;
                return -1;
            }
            if (old_dentry->get_type() == HSCFS_FT_DIR)
            {
                errno = EISDIR;
                return -1;
            }

            /* 检查new_path的目录，目录必须存在 */
            std::string new_dir = path_helper::extract_dir_path(new_path);
            path_lookup_proc.set_abs_path(new_dir);
            dentry_handle new_dir_dentry = path_lookup_proc.do_path_lookup();
            if (!new_dir_dentry.is_exist())
            {
                errno = ENOENT;
                return -1;
            }
            if (new_dir_dentry->get_type() != HSCFS_FT_DIR)
            {
                errno = ENONET;
                return -1;
            }
            
            /* 检查new_path对应文件，不应该存在 */
            std::string new_file = path_helper::extract_file_name(new_path);
            path_lookup_proc.set_rel_path(new_dir_dentry, new_file);
            dentry_store_pos create_pos_hint;
            dentry_handle new_file_dentry = path_lookup_proc.do_path_lookup(&create_pos_hint);
            if (new_file_dentry.is_exist())
            {
                errno = EEXIST;
                return -1;
            }

            /* 将old_dentry的nlink加1 */
            uint32_t nlink = file_nlink_utils(fs_manager).add_nlink(old_dentry->get_ino());
            HSCFS_LOG(HSCFS_LOG_INFO, "add nlink to %s, now its nlink equals %u.", old_path.c_str(), nlink);

            /* 创建new_path的目录项 */
            directory new_file_dir(new_dir_dentry, fs_manager);
            new_file_dir.link(new_file, old_dentry->get_ino(), &create_pos_hint);

            return 0;
        }
        catch(const std::exception& e)
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

extern "C" int link(const char *oldpath, const char *newpath)
{
    return hscfs::link(oldpath, newpath);
}

#endif