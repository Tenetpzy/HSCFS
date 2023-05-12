#include "fs/fd_array.hh"
#include "fs/fs_manager.hh"
#include "fs/opened_file.hh"
#include "utils/lock_guards.hh"
#include "utils/exception_handler.hh"

namespace hscfs {

ssize_t write(int fd, void *buffer, size_t count)
{
    file_system_manager *fs_manager = file_system_manager::get_instance();
    try
    {
        rwlock_guard fs_freeze_lg(fs_manager->get_fs_freeze_lock(), rwlock_guard::lock_type::rdlock);
        opened_file *file = fs_manager->get_fd_array()->get_opened_file_of_fd(fd);
        return file->write(static_cast<char*>(buffer), count);
    }
    catch (std::exception& e)
    {
        errno = exception_handler(fs_manager, e).convert_to_errno();
        return -1;
    }
}

}  // namespace hscfs

#ifdef CONFIG_C_API

extern "C" ssize_t write(int fd, void *buffer, size_t count)
{
    return hscfs::write(fd, buffer, count);
}

#endif 