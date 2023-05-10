#include "fs/opened_file.hh"
#include "fs/open_flags.hh"
#include "utils/hscfs_exceptions.hh"

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
    assert(count >= 0);
    
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