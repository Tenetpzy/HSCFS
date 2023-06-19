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

    /* 
     * 构造opened_file并关联file结构，调用者需持有fs_meta_lock
     * 内部将增加file和dentry的fd引用计数
     */
    opened_file(uint32_t flags, const file_handle &file_);

    /*
     * ~opened_file()
     * 析构时由系统控制，减少file和dentry的fd引用计数，不在析构函数中减少
     */

    file_handle& get_file_handle() noexcept
    {
        return file;
    }

    /* 
     * 读文件 
     * 调用者应持有fs_freeze_lock共享锁
     */
    ssize_t read(char *buffer, ssize_t count);

    /* 
     * 写文件
     * 调用者应持有fs_freeze_lock共享锁 
     */
    ssize_t write(char *buffer, ssize_t count);

    /* 
     * 更改读写位置
     * 调用者应持有fs_freeze_lock共享锁 
     */
    off_t set_rw_pos(off_t offset, int whence);

private:
    uint32_t flags;  // 打开文件时的flags
    uint64_t pos;  // 当前文件读写位置
    std::mutex pos_lock;  // 保护pos的锁
    file_handle file;

    enum class rw_operation {
        read, write
    };

    /* 检查flags是否支持op操作 */
    void rw_check_flags(rw_operation op);
};

}