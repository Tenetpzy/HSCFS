#include "fs/fs_manager.hh"
#include "fs/fd_array.hh"
#include "fs/opened_file.hh"
#include "utils/exception_handler.hh"
#include "utils/lock_guards.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

int close(int fd)
{
    file_system_manager *fs_manager = file_system_manager::get_instance();
    try
    {
        rwlock_guard fs_freeze_lg(fs_manager->get_fs_freeze_lock(), rwlock_guard::lock_type::rdlock);
        std::lock_guard<std::mutex> fs_meta_lg(fs_manager->get_fs_meta_lock());
        fs_manager->check_state();

        try
        {
            std::shared_ptr<opened_file> o_file = fs_manager->get_fd_array()->free_fd(fd);
            file_handle &file = o_file->get_file_handle();
            file->sub_fd_refcount();

            /* 如果文件需要被删除(fd ref和nlink都为0)，则在此处删除文件 */
            if (file->get_fd_refcount() == 0 && file->get_nlink() == 0)
            {
                HSCFS_LOG(HSCFS_LOG_INFO, "delete file(inode = %u) when close its last fd.", file->get_inode());
                file.delete_file();
            }

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

extern "C" int close(int fd)
{
    return hscfs::close(fd);
}

#endif