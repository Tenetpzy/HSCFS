#pragma once

#include <stdexcept>

namespace hscfs {

// 若调用线程库，则直接抛出std::system_error

class alloc_error: public std::bad_alloc
{
public:
    alloc_error(const std::string &s) : std::bad_alloc() {
        msg = s;
    }

    const char* what() const noexcept override {
        return msg.c_str();
    }

private:
    std::string msg;
};

class io_error: public std::runtime_error
{
public:
    io_error(const std::string &s) : std::runtime_error(s) {}
    io_error(const char *s) : std::runtime_error(s) {}
};

class timer_error: public std::runtime_error
{
public:
    timer_error(const std::string &s) : std::runtime_error(s) {}
    timer_error(const char *s) : std::runtime_error(s) {}
};

class thread_interrupted {};

class not_recoverable: public std::runtime_error
{
public:
    not_recoverable(): std::runtime_error("not recoverable.") {}
};

class expand_file_size_exceed_limit: public std::logic_error
{
public:
    expand_file_size_exceed_limit(const char *s): std::logic_error(s) {}
};

class no_free_nid: public std::runtime_error
{
public:
    no_free_nid(): std::runtime_error("no free nid to alloc.") {}
};

class no_free_segment: public std::runtime_error
{
public:
    no_free_segment(): std::runtime_error("no free segment to alloc.") {}
}

/* 以上均为不可恢复异常 */
/**************************************************************/

/* 用户调用API时输入的路径字符串不合法 */
class user_path_invalid: public std::logic_error 
{
public:
    user_path_invalid(const char *s): std::logic_error(s) {}
};

}  // namespace hscfs