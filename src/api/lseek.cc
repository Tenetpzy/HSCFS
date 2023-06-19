#include "fs/opened_file.hh"
#include "fs/fd_array.hh"
#include "fs/fs_manager.hh"
#include "utils/lock_guards.hh"
#include "utils/exception_handler.hh"

namespace hscfs {

off_t lseek(int fd, off_t offset, int whence)
{
    file_system_manager *fs_manager = file_system_manager::get_instance();
    try
    {
        rwlock_guard fs_freeze_lg(fs_manager->get_fs_freeze_lock(), rwlock_guard::lock_type::rdlock);
        try
        {
            opened_file *file = fs_manager->get_fd_array()->get_opened_file_of_fd(fd);
            return file->set_rw_pos(offset, whence);
        }
        catch (const std::exception &e)
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

extern "C" off_t lseek(int fd, off_t offset, int whence)
{
    return hscfs::lseek(fd, offset, whence);
}

#endif
