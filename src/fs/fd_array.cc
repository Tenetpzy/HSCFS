#include "fs/fd_array.hh"
#include "utils/hscfs_log.h"
#include "utils/lock_guards.hh"
#include <system_error>
#include <cassert>
#include "fd_array.hh"

namespace hscfs {

fd_array::fd_array(size_t size)
{
    int ret = spin_init(&lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "fd array: init spin lock failed.");
    fd_arr.resize(std::max(static_cast<size_t>(3), size));
    alloc_pos = 3;   // 保留fd 0, 1, 2不分配
}

fd_array::~fd_array()
{
    int ret = spin_destroy(&lock);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "fd array: destroy spin lock failed.");
}

int fd_array::alloc_fd(std::shared_ptr<opened_file> &p_file)
{
    spin_lock_guard lg(lock);
    int ret;
    if (!free_set.empty())
    {
        ret = *free_set.begin();
        free_set.erase(free_set.begin());
    }
    else
    {
        assert(alloc_pos <= fd_arr.size());
        if (alloc_pos == fd_arr.size())
            fd_arr.resize(fd_arr.size() * 2);
        ret = alloc_pos++;
    }
    fd_arr[ret] = p_file;
    return ret;
}

void fd_array::free_fd(int fd)
{
    spin_lock_guard lg(lock);
    free_set.insert(fd);
    fd_arr[fd] = nullptr;
}

}