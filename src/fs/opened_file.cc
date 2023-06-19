#include "fs/opened_file.hh"
#include "fs/open_flags.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/lock_guards.hh"

namespace hscfs {

opened_file::opened_file(uint32_t flags, const file_handle &file_)
    : file(file_)
{
    this->flags = flags;
    pos = 0;
    file->add_fd_refcount();
}

ssize_t opened_file::read(char *buffer, ssize_t count)
{
    std::lock_guard<std::mutex> lg(pos_lock);
    rw_check_flags(rw_operation::read);
    if (count < 0)
        count = 0;
    ssize_t num_read;
    {
        /* 获得文件操作共享锁，获取后，文件长度保证不会减小(truncate需要获取此独占锁) */
        rwlock_guard file_op_lg(file->get_file_op_lock(), rwlock_guard::lock_type::rdlock);
        num_read = file->read(buffer, count, pos);
    }
    file.mark_dirty();
    pos += num_read;
    return num_read;
}

ssize_t opened_file::write(char *buffer, ssize_t count)
{
    std::lock_guard<std::mutex> lg(pos_lock);
    rw_check_flags(rw_operation::write);
    if (count < 0)
        count = 0;
    ssize_t num_write;
    {
        if (flags & O_APPEND)  // 如果是APPEND写
        {
            rwlock_guard file_op_lg(file->get_file_op_lock(), rwlock_guard::lock_type::wrlock);
            pos = file->get_cur_size();  // 将写入位置设置为文件当前的尾后位置
            num_write = file->write(buffer, count, pos);
        }
        else
        {
            rwlock_guard file_op_lg(file->get_file_op_lock(), rwlock_guard::lock_type::rdlock);
            num_write = file->write(buffer, count, pos);
        }
    }
    file.mark_dirty();
    pos += num_write;
    return num_write;    
}

off_t opened_file::set_rw_pos(off_t offset, int whence)
{
    std::lock_guard<std::mutex> lg(pos_lock);
    switch (whence)
    {
    case SEEK_SET:
        pos = offset;
        break;
    
    case SEEK_CUR:
        pos += offset;
        break;
    
    case SEEK_END:
        pos = file->get_cur_size() + offset;
        break;

    default:
        break;
    }

    return pos;
}

void opened_file::rw_check_flags(rw_operation op)
{
    uint32_t rw_flag = flags & 3U;
    if (op == rw_operation::read)
    {
        if (rw_flag == O_WRONLY)
            throw rw_conflict_with_open_flag("can not read on write only fd.");
    }
    else  // op == rw_operation::write
    {
        if (rw_flag == O_RDONLY)
            throw rw_conflict_with_open_flag("can not write on read only fd.");
    }
}

} // namespace hscfs