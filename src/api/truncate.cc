#include "fs/fd_array.hh"
#include "fs/file.hh"
#include "fs/fs_manager.hh"
#include "fs/opened_file.hh"
#include "utils/lock_guards.hh"
#include "utils/exception_handler.hh"

namespace hscfs {

int truncate(int fd, off_t length)
{
    file_system_manager *fs_manager = file_system_manager::get_instance();
    try
    {
        rwlock_guard fs_freeze_lg(fs_manager->get_fs_freeze_lock(), rwlock_guard::lock_type::rdlock);
        try
        {
            opened_file *file = fs_manager->get_fd_array()->get_opened_file_of_fd(fd);
            file_handle &handle = file->get_file_handle();
            rwlock_guard file_op_lg(handle->get_file_op_lock(), rwlock_guard::lock_type::wrlock);
            std::lock_guard<std::mutex> fs_meta_lg(fs_manager->get_fs_meta_lock());
            if (handle->truncate(length))
                handle.mark_dirty();
            return 0;
        }
        catch (std::exception &e)
        {
            errno = exception_handler(fs_manager, e).convert_to_errno(true);
            return -1;
        }
    }
    catch (std::exception &e)
    {
        errno = exception_handler(fs_manager, e).convert_to_errno();
        return -1;
    }
}

}  // namespace hscfs


#ifdef CONFIG_C_API

extern "C" int truncate(int fd, off_t length);

int truncate(int fd, off_t length)
{
    return hscfs::truncate(fd, length);
}

#endif