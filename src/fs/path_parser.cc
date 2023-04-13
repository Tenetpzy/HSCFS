#include "fs/path_parser.hh"

namespace hscfs {

const char* const path_parser::hscfs_prefix = "hscfs:";

path_dentry_iterator::path_dentry_iterator(path_parser &p_parser, size_t start_idx)
    : path_name(p_parser.path_name)
{
    path_len = p_parser.path_len;
    cur_idx = start_idx;
}

std::string path_dentry_iterator::next()
{
    size_t idx = cur_idx;
    while (idx < path_len && path_name[idx] != '/')
        ++idx;
    std::string res(path_name, idx - cur_idx);

    // 跳过接下来的所有'/'
    while (idx < path_len && path_name[idx] == '/')
        ++idx;
    cur_idx = idx;
    return res;
}

}  // namespace hscfs