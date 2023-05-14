#pragma once

#include <memory>
#include <vector>
#include <set>

#include "utils/hscfs_multithread.h"

namespace hscfs {

class opened_file;

class fd_array
{
public:
    fd_array(size_t size);
    ~fd_array();

    int alloc_fd(std::shared_ptr<opened_file> &p_file);
    void free_fd(int fd);
    opened_file* get_opened_file_of_fd(int fd);

private:
    /* 
     * 在fd_arr中，[alloc_pos, fd_arr.size())之间的fd没有被分配
     * [0, alloc_pos)之间的可用fd存放在free_set中
     * 
     * 分配fd时，优先从free_set中选取最小可用fd分配
     * 若free_set为空，则从alloc_pos分配，并将其+1。若alloc_pos == fd_arr.size()，则二倍扩容fd_arr再分配。
     * 释放fd时，将其加入free_set
     */
    std::vector<std::shared_ptr<opened_file>> fd_arr;
    size_t alloc_pos;
    std::set<int> free_set;
    spinlock_t lock;
};

}