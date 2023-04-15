#pragma once

#include "fs/path_utils.hh"

#include <memory>

namespace hscfs {

class path_lookup_buf_deletor
{
public:
    void operator()(char *buf);
};

class ssd_path_lookup_controller
{
public:
    
    /* 构造SSD路径解析命令 */
    void construct_task(const path_parser &path_parser, uint32_t start_ino, path_dentry_iterator start_itr);
private:
    
    std::unique_ptr<char, path_lookup_buf_deletor> p_task_buf;
    std::unique_ptr<char, path_lookup_buf_deletor> p_task_res_buf;
};

}  // namespace hscfs