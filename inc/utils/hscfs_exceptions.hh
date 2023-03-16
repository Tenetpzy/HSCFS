#pragma once

#include <stdexcept>

class hscfs_alloc_error: public std::bad_alloc
{
public:
    hscfs_alloc_error(const std::string &s) : std::bad_alloc() {
        msg = s;
    }

    const char* what() const noexcept override {
        return msg.c_str();
    }

private:
    std::string msg;
};

class hscfs_io_error: public std::runtime_error
{
public:
    hscfs_io_error(const std::string &s) : std::runtime_error(s) {}
    hscfs_io_error(const char *s) : std::runtime_error(s) {}
};

class hscfs_timer_error: public std::runtime_error
{
public:
    hscfs_timer_error(const std::string &s) : std::runtime_error(s) {}
    hscfs_timer_error(const char *s) : std::runtime_error(s) {}
};