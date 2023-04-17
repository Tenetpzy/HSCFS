#include "cache/super_cache.hh"
#include "cache/dentry_cache.hh"
#include "fs/fs_manager.hh"
#include "fs/path_utils.hh"

namespace hscfs {

int open(const char *pathname, int flags)
{
    std::string abs_path = path_helper::extract_abs_path(pathname);
    std::string dir_path = path_helper::extract_dir_path(abs_path);
    std::string file_name = path_helper::extract_file_name(abs_path);

    file_system_manager *fs_manager = file_system_manager::get_instance();
    std::lock_guard<std::mutex> lg(fs_manager->get_fs_lock_ref());
    
    /* 找到目标文件的目录的dentry */
    path_lookup_processor proc(fs_manager);
    proc.set_abs_path(dir_path);
    dentry_handle dir_dentry = proc.do_path_lookup();

    /* to do */
    if (dir_dentry.is_empty())
    {
        /* 返回错误，原因ENOENT */
    }

    /* 找到目标文件的dentry */
    proc.set_rel_path(dir_dentry, file_name);
    dentry_handle target_dentry = proc.do_path_lookup();

    if (target_dentry.is_empty())
    {
        /* 查看是否有O_CREATE标志，做相应处理 */
    }

    if (target_dentry->get_type() != HSCFS_FT_REG_FILE)
    {
        /* 目标不是文件，返回错误，原因EISDIR */
    }

    /* 处理打开的其它标志，处理opened_file和fd */

    return 0;
}

}


#ifdef CONFIG_C_API

extern "C" int open(const char *pathname, int flags, ...);

int open(const char *pathname, int flags, ...)
{
    return hscfs::open(pathname, flags);
}

#endif