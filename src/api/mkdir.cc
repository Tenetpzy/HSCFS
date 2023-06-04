#include "cache/dentry_cache.hh"
#include "fs/fs_manager.hh"
#include "fs/path_utils.hh"
#include "fs/fs.h"
#include "fs/directory.hh"
#include "utils/lock_guards.hh"
#include "utils/exception_handler.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

int mkdir(const char *pathname)
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
            std::string dir_path = path_helper::extract_dir_path(abs_path);
            std::string file = path_helper::extract_file_name(abs_path);

            if (dir_path.length() == 0 || file.length() == 0)
            {
                errno = EINVAL;
                return -1;
            }

            path_lookup_processor path_lookup_proc(fs_manager);
            
            path_lookup_proc.set_abs_path(dir_path);
            dentry_handle dir_dentry = path_lookup_proc.do_path_lookup();
            if (!dir_dentry.is_exist())
            {
                errno = ENOENT;
                return -1;
            }
            if (dir_dentry->get_type() != HSCFS_FT_DIR)  // 不能与上一个条件合并，因为dir_entry可能为空
            {
                errno = ENOENT;
                return -1;
            }

            path_lookup_proc.set_rel_path(dir_dentry, file);
            dentry_store_pos create_pos_hint;
            dentry_handle target_dentry = path_lookup_proc.do_path_lookup(&create_pos_hint);
            if (target_dentry.is_exist())
            {
                errno = EEXIST;
                return -1;
            }

            HSCFS_LOG(HSCFS_LOG_DEBUG, "creating directory %s.", abs_path.c_str());
            directory dir(dir_dentry, fs_manager);
            dir.create(file, HSCFS_FT_DIR, &create_pos_hint);

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

#include <sys/stat.h>

extern "C" int mkdir(const char *pathname, mode_t mode)
{
    return hscfs::mkdir(pathname);
}

#endif