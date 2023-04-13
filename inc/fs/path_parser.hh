#pragma once

#include <cstring>
#include <string>

namespace hscfs {

class path_parser;

class path_dentry_iterator
{
public:
    /*
     * 由path_parser构造，start_idx需指向某个目录项的起始字符('/'后的第一个字符)，或为path_len（表示结尾）
     */
    path_dentry_iterator(path_parser &p_parser, size_t start_idx);

    /* 
     * 返回当前的目录项名，并让iterator指向下一项 
     */
    std::string next();

    bool operator==(const path_dentry_iterator&o) const noexcept
    {
        return path_name == o.path_name && cur_idx == o.cur_idx;
    }

private:
    const char *const path_name;
    size_t path_len;
    size_t cur_idx;
};

/*
 * 路径字符串解析器
 * 文件名（目录项名）可以包含除了'/'和'\0'外的所有字符
 * 用户使用的路径字符串必须是如下格式："${hscfs_prefix}/*"。
 */
class path_parser
{
public:
    path_parser(const char *path) : path_name(path) 
    {
        path_len = std::strlen(path_name);
    }

    path_dentry_iterator begin();
    path_dentry_iterator end();

private:
    const char *const path_name;
    size_t path_len;

    static const char* const hscfs_prefix;

    friend class path_dentry_iterator;
};

}