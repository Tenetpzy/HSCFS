#pragma once

#include "fs/path_utils.hh"
#include <memory>

struct comm_dev;

namespace hscfs {

class path_lookup_buf_deletor
{
public:
    void operator()(char *buf);
};

class ssd_path_lookup_controller
{
public:
    ssd_path_lookup_controller(comm_dev *device)
    {
        dev = device;
    }

    /* 构造SSD路径解析命令 */
    void construct_task(const path_parser &path_parser, uint32_t start_ino, path_dentry_iterator start_itr);

    /* 进行路径解析，将结果保存到内部buf中 */
    void do_pathlookup();

    /* 返回结果中ino表的首元素地址 */
    uint32_t* get_first_addr_of_result_inos() const noexcept;
    
    /* to do */

private:
    comm_dev *dev;
    std::unique_ptr<char, path_lookup_buf_deletor> p_task_buf;
    std::unique_ptr<char, path_lookup_buf_deletor> p_task_res_buf;
};

}  // namespace hscfs