#pragma once

#include <mutex>
#include <cstdint>
#include "fs/file.hh"

namespace hscfs {

class file;

/* 描述一个打开的文件，类似VFS的file */
class opened_file
{
public:
    opened_file(uint32_t flags, uint32_t mode, file *file)
    {
        this->flags = flags;
        this->mode = mode;
        pos = 0;
        this->file = file;
    }

    /* 读文件 */
    ssize_t read(void *buffer, size_t count);

    /* 写文件 */
    ssize_t write(void *buffer, size_t count);

private:
    uint32_t flags;  // 打开文件时的flags
    uint32_t mode;  // 打开文件的mode
    uint64_t pos;  // 当前文件读写位置

    /* 
     * 指向file对象
     * opened_file对象析构需要上层主动调用close，
     * 且close时需要加fs_meta_lock，为避免死锁，此处不使用智能指针自动管理
     */
    file* file;
    std::mutex pos_lock;  // 保护pos的锁
};

}