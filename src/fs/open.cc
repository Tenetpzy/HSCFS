#include "cache/super_cache.hh"
#include "cache/dentry_cache.hh"
#include "fs/fs_manager.hh"
#include "fs/path_utils.hh"
#include "fs/open_flags.hh"
#include "fs/directory.hh"
#include "fs/file.hh"
#include "fs/fd_array.hh"
#include "fs/opened_file.hh"
#include "utils/lock_guards.hh"
#include "utils/exception_handler.hh"

namespace hscfs {

int do_open(const char *pathname, int flags)
{
    try
    {
        file_system_manager *fs_manager = file_system_manager::get_instance();
        std::string abs_path = path_helper::extract_abs_path(pathname);
        std::string dir_path = path_helper::extract_dir_path(abs_path);
        std::string file_name = path_helper::extract_file_name(abs_path);

        /* 找到目标文件的目录的dentry */
        path_lookup_processor proc(fs_manager);
        proc.set_abs_path(dir_path);
        dentry_handle dir_dentry = proc.do_path_lookup();

        /* 目录不存在或不是目录 */
        if (dir_dentry.is_empty() || dir_dentry->get_type() != HSCFS_FT_DIR)
        {
            errno = ENOENT;
            return -1;
        }

        /* 找到目标文件的dentry */
        proc.set_rel_path(dir_dentry, file_name);
        dentry_store_pos target_pos_hint;
        dentry_handle target_dentry = proc.do_path_lookup(&target_pos_hint);

        /* 如果目标文件不存在，查看是否有O_CREATE标志 */
        if (target_dentry.is_empty() || target_dentry->get_state() == dentry_state::deleted)
        {
            /* 如果有O_CREAT，则创建该文件 */
            if (flags | O_CREAT)
            {
                directory dir_helper(dir_dentry, fs_manager);

                /* 如果dir_dentry已经创建过文件但没写回，则target_pos_hint有可能不正确 */
                target_dentry = dir_helper.create(file_name, HSCFS_FT_REG_FILE, &target_pos_hint);
            }
            else
            {
                errno = ENOENT;
                return -1;
            }
        }

        /* 如果目标文件存在，判断其它错误情况 */
        else
        {
            /* 如果目标文件不是普通文件，返回错误 */
            if (target_dentry->get_type() != HSCFS_FT_REG_FILE)
            {
                errno = EISDIR;
                return -1;
            }

            /* 如果目标文件已被删除，但仍然被fd引用，则不允许打开或创建该文件 */
            if (target_dentry->get_state() == dentry_state::deleted_inode_valid)
            {
                errno = EACCES;
                return -1;
            }
        }

        /* 此时目标文件一定存在，获得目标文件的file对象 */
        file_handle file = file_cache_helper(fs_manager->get_file_obj_cache())
            .get_file_obj(target_dentry->get_ino(), target_dentry);

        /* 如果带有O_TRUNC标志，则清空文件内容 */
        if (flags | O_TRUNC)
            file->truncate(0);

        /* 分配fd和opened_file结构 */
        auto p_opened_file = std::make_shared<opened_file>(flags, file);
        fd_array *fds = fs_manager->get_fd_array();
        int fd = fds->alloc_fd(p_opened_file);
        
        return fd;
    }
    catch (std::exception &e)
    {
        int err = exception_handler(e).convert_to_errno(true);
        errno = err;
        return -1;
    }
}

/*
 * 打开一个文件，返回其fd
 * 
 * 若出错，返回-1，置errno为：
 * EINVAL：pathname或flags不合法
 * ENOENT：路径中某个目录项不存在
 * EISDIR：试图打开目录文件
 * EACCES：目标文件已经被删除，但此时系统中仍有引用，不允许再次打开或创建同名文件
 * ENOTRECOVERABLE：文件系统出现内部错误，无法恢复正常状态
 */
int open(const char *pathname, int flags)
{
    try 
    {
        file_system_manager *fs_manager = file_system_manager::get_instance();
        rwlock_guard lg1(fs_manager->get_fs_freeze_lock(), rwlock_guard::lock_type::rdlock);
        std::lock_guard<std::mutex> lg2(fs_manager->get_fs_meta_lock());
        fs_manager->check_state();

        return do_open(pathname, flags);
    } 
    catch (const std::exception &e) 
    {
        errno = exception_handler(e).convert_to_errno();
        return -1;
    }
}

}  // namespace hscfs


#ifdef CONFIG_C_API

extern "C" int open(const char *pathname, int flags, ...);

int open(const char *pathname, int flags, ...)
{
    return hscfs::open(pathname, flags);
}

#endif