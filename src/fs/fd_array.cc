#include "fs/fd_array.hh"
#include "utils/hscfs_log.h"
#include "utils/lock_guards.hh"
#include "utils/hscfs_exceptions.hh"
#include <system_error>
#include <cassert>

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
    HSCFS_LOG(HSCFS_LOG_INFO, "allocate fd %d.", ret);
    fd_arr[ret] = p_file;
    unclosed_fds.insert(ret);
    return ret;
}

std::shared_ptr<opened_file> fd_array::free_fd(int fd)
{
    spin_lock_guard lg(lock);
    if (static_cast<size_t>(fd) >= fd_arr.size() || fd_arr[fd] == nullptr)
        throw invalid_fd();
    std::shared_ptr<opened_file> ret = fd_arr[fd];
    fd_arr[fd] = nullptr;
    free_set.insert(fd);
    unclosed_fds.erase(fd);
    HSCFS_LOG(HSCFS_LOG_INFO, "free fd %d.", fd);
    return ret;
}

opened_file* fd_array::get_opened_file_of_fd(int fd)
{
    spin_lock_guard lg(lock);
    if (static_cast<size_t>(fd) >= fd_arr.size() || fd_arr[fd] == nullptr)
        throw invalid_fd();
    return fd_arr[fd].get();
}

std::unordered_set<int> fd_array::get_and_clear_unclosed_fds()
{
    std::unordered_set<int> ret;
    unclosed_fds.swap(ret);
    assert(unclosed_fds.empty());
    return ret;
}

} // namespace hscfs