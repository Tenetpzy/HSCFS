#include "cache/super_cache.hh"
#include "fs/fs_manager.hh"
#include "fs/path_utils.hh"

namespace hscfs {

int open(const char *pathname, int flags)
{
    std::string abs_path = path_helper::extract_abs_path(pathname);
    std::string dir_path = path_helper::extract_dir_path(abs_path);
    std::string file_name = path_helper::extract_file_name(abs_path);

    std::lock_guard<std::mutex> lg(file_system_manager::get_instance()->get_fs_lock_ref());


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