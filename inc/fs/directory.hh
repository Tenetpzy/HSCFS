#pragma once

#include <cstdint>
#include <string>
#include "cache/dentry_cache.hh"

namespace hscfs {

class file_system_manager;

/* 保存一个目录项的信息，内部使用 */
struct dentry_info
{
    uint32_t ino;  // inode号
    uint32_t blkno, slotno;  // 存储位置
    uint8_t type;  // 类型
    bool is_valid;  // 以上信息是否有效
};

class directory
{
public:
    directory(const dentry_handle &dentry, file_system_manager *fs_manager) noexcept
    {
        this->ino = dentry->get_ino();
        this->dentry = dentry;
        this->fs_manager = fs_manager;
    }

    /*
     * 创建一个文件，文件名为name，文件类型为type
     * 将为新文件分配inode，创建inode block并加入node block缓存
     * 创建新文件的目录项，将其加入dentry cache，并将其返回
     * 调用者可以提供创建目录项的位置信息(create_pos_hint)
     * 
     * 不应该在已经存在同名目录项name时调用create，否则结果未定义
     * 
     * 调用者需持有fs_meta_lock 
     */
    dentry_handle create(const std::string &name, uint8_t type, const dentry_store_pos *create_pos_hint = nullptr);

    /*
     * 在目录文件中查找目录项name
     * 返回该目录项的句柄，目录项中位置信息有效
     * 
     * 若目录项不存在，则handle的is_empty返回true，
     * 且handle的位置信息包含如果要创建该文件，则应创建的位置(除非当前目录文件空闲空间中放不下该文件目录项了)
     * 
     * 调用者需持有fs_meta_lock
     */
    dentry_handle lookup(const std::string &name);

private:
    uint32_t ino;  // 目录文件的ino
    dentry_handle dentry;  // 对应的dentry
    file_system_manager *fs_manager;

    dentry_info lookup_in_dirfile(const std::string &name);
};

}  // namespace hscfs