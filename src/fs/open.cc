#include "cache/super_cache.hh"
#include "cache/dentry_cache.hh"
#include "fs/fs_manager.hh"
#include "fs/path_utils.hh"
#include "utils/lock_guards.hh"
#include "utils/exception_handler.hh"

namespace hscfs {

/*
 * 打开一个文件，返回其fd
 * 
 * 若出错，返回-1，置errno为：
 * EINVAL：pathname或flags不合法
 * ENOENT：路径中某个目录项不存在
 * EISDIR：试图打开目录文件
 * 
 * 
 */
int open(const char *pathname, int flags)
{
    int ret = 0;
    try {

    file_system_manager *fs_manager = file_system_manager::get_instance();
    rwlock_guard lg1(fs_manager->get_fs_freeze_lock(), rwlock_guard::lock_type::rdlock);
    std::lock_guard<std::mutex> lg2(fs_manager->get_fs_meta_lock());

    std::string abs_path = path_helper::extract_abs_path(pathname);
    std::string dir_path = path_helper::extract_dir_path(abs_path);
    std::string file_name = path_helper::extract_file_name(abs_path);

    /* 找到目标文件的目录的dentry */
    path_lookup_processor proc(fs_manager);
    proc.set_abs_path(dir_path);
    dentry_handle dir_dentry = proc.do_path_lookup();

    /* 目录不存在 */
    if (dir_dentry.is_empty())
    {
        errno = ENOENT;
        return -1;
    }

    /* 找到目标文件的dentry */
    proc.set_rel_path(dir_dentry, file_name);
    dentry_handle target_dentry = proc.do_path_lookup();

    /* 目标文件存在但不是普通文件，不能使用open打开 */
    if (!target_dentry.is_empty() && target_dentry->get_type() != HSCFS_FT_REG_FILE)
    {
        errno = EISDIR;
        return -1;
    }

    if (target_dentry.is_empty())
    {
        
    }

    /* 处理打开的其它标志，处理opened_file和fd */

    } catch (const std::exception &e) {
        errno = handle_exception(e);
        ret = -1;
    }

    return ret;
}

}  // namespace hscfs


#ifdef CONFIG_C_API

extern "C" int open(const char *pathname, int flags, ...);

int open(const char *pathname, int flags, ...)
{
    return hscfs::open(pathname, flags);
}

#endif