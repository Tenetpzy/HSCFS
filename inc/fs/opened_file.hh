#pragma once

#include <mutex>
#include <cstdint>
#include "fs/file.hh"

namespace hscfs {

/* 描述一个打开的文件，类似VFS的file */
class opened_file
{
public:
    opened_file(uint32_t flags, uint32_t mode, file_handle &&f_handle)
        : file(std::move(f_handle))
    {
        this->flags = flags;
        this->mode = mode;
        pos = 0;
    }

private:
    uint32_t flags;  // 打开文件时的flags
    uint32_t mode;  // 打开文件的mode
    uint64_t pos;  // 当前文件读写位置
    file_handle file;  // 指向file对象
    std::mutex pos_lock;  // 保护pos的锁
};

}