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