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

    /* 构造opened_file，不关联file结构 */
    opened_file(uint32_t flags)
    {
        this->flags = flags;
        pos = 0;
    }

    opened_file(uint32_t flags, const file_handle &file_)
        : file(file_) 
    {
        this->flags = flags;
        pos = 0;
    }

    void relate_file(const file_handle &file_)
    {
        file = file_;
    }

    /* 读文件 */
    ssize_t read(void *buffer, size_t count);

    /* 写文件 */
    ssize_t write(void *buffer, size_t count);

private:
    uint32_t flags;  // 打开文件时的flags
    uint64_t pos;  // 当前文件读写位置

    /* 
     * 指向file对象
     * opened_file对象析构需要上层主动调用close，
     * 且close时需要加fs_meta_lock，为避免死锁，此处不使用智能指针自动管理
     */
    file_handle file;
    std::mutex pos_lock;  // 保护pos的锁
};

}