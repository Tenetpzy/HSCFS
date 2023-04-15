#pragma once

#include <cstring>
#include <string>
#include "cache/dentry_cache.hh"

namespace hscfs {

class path_parser;

class path_dentry_iterator
{
public:
    /*
     * 由path_parser构造，start_idx需指向某个目录项的起始字符或path的尾后字符(表示end迭代器)
     * 起始字符定义为目录项前的'/'或目录项的首字符
     */
    path_dentry_iterator(const std::string &path, size_t start_pos)
        : path_(path) 
    {
        cur_pos = start_pos; 
    }

    /* 
     * 返回当前的目录项名，并让iterator指向下一项
     */
    void next();

    /*
     * 返回当前的目录项名
     */
    std::string get();

    bool operator==(const path_dentry_iterator &o) const noexcept
    {
        return &path_ == &o.path_ && is_pos_equal(cur_pos, o.cur_pos);
    }

    bool operator!=(const path_dentry_iterator &o) const noexcept
    {
        return !(*this == o);
    }

private:
    const std::string &path_;
    size_t cur_pos;
    size_t nxt_start_pos = std::string::npos;

    /* 判断两个pos是否指向同一个目录项。（消除'/'的影响） */
    bool is_pos_equal(size_t pos1, size_t pos2) const;
};

/*
 * 路径字符串解析器
 * 文件名（目录项名）可以包含除了'/'和'\0'外的所有字符
 */
class path_parser
{
public:
    path_parser(const std::string &path) : path_(path) {}

    path_dentry_iterator begin() const noexcept
    {
        return path_dentry_iterator(path_, 0);
    }

    path_dentry_iterator end() const noexcept
    {
        return path_dentry_iterator(path_, path_.length());
    }

private:
    const std::string path_;
};

class path_helper
{
public:

    /*
     * 提取用户调用API时提供的路径字符串中的标准的绝对路径
     * 
     * 若配置了CONFIG_PATH_PREFIX，则用户提供的路径字符串应是 "${CONFIG_PATH_PREFIX}" + abs_path，
     * 否则抛出user_path_invalid异常
     * 
     * 若没有配置CONFIG_PATH_PREFIX，则用户提供的路径应是绝对路径，否则抛出user_path_invalid异常
     */
    static std::string extract_abs_path(const char *user_path);

    /* 
     * 提取路径中的目录路径
     * 返回的结果中，以'/'结尾
     * path应是标准的绝对路径或相对路径的形式
     */
    static std::string extract_dir_path(const std::string &path);

    /* 
     * 提取路径中的目标文件名 
     * 如果path以'/'结尾，则认为其为路径名而非文件名，返回的string.length()为0
     * path应是标准的绝对路径或相对路径的形式
     */
    static std::string extract_file_name(const std::string &path);

private:
    static bool is_prefix_valid(const std::string& user_path, const std::string &prefix);
};


class file_system_manager;

/* 
 * 路径解析的执行器
 * 使用此类之前，必须获得fs_lock 
 */
class path_lookup_processor
{
public:
    path_lookup_processor(file_system_manager *fs_manager)
    {
        this->fs_manager = fs_manager;
    }

    void set_abs_path(const std::string &abs_path);

    void set_rel_path(const dentry_handle &start_dir_dentry, const std::string &rel_path)
    {
        start_dentry = start_dir_dentry;
        path = rel_path;
    }

    /*
     * 对set_abs_path中传入的path进行路径解析
     * 返回最后一级目录下的dentry handle
     * 若某一级目录项不存在，则返回的handle的is_empty方法返回true
     */
    dentry_handle do_path_lookup();

private:
    file_system_manager *fs_manager;
    std::string path;
    dentry_handle start_dentry;
};

}