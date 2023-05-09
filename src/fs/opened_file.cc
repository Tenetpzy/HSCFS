#include "fs/opened_file.hh"

namespace hscfs {

opened_file::opened_file(uint32_t flags, const file_handle &file_)
    : file(file_)
{
    this->flags = flags;
    pos = 0;
    file.add_fd_refcount();
}

} // namespace hscfs