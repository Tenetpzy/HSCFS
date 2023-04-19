#pragma once

#include <cstdint>
#include <string>
#include "cache/dentry_cache.hh"

namespace hscfs {

class file_system_manager;

class directory
{
public:
    directory(uint32_t ino, file_system_manager *fs_manager) noexcept
    {
        this->ino = ino;
        this->fs_manager = fs_manager;
    }

    /*
     * 创建一个文件，文件名为name，文件类型为type
     * 将为新文件分配inode，创建inode block并加入node block缓存
     * 返回：<新文件的inode号，新文件的目录项在目录文件的块号，目录项在块内的slot号>
     * 调用者需持有fs_meta_lock 
     */
    dentry_handle create(const std::string &name, uint8_t type);

private:
    uint32_t ino;
    file_system_manager *fs_manager;
};

}  // namespace hscfs