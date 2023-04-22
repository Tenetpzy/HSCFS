#pragma once

#include <cstdint>
#include <string>
#include "cache/dentry_cache.hh"

namespace hscfs {

class file_system_manager;

/* 保存一个目录项的信息，内部使用 */
struct dentry_info
{
    uint32_t ino;  // inode号，无效则为INVALID_NID
    uint8_t type;  // 类型
    dentry_store_pos store_pos;  // 存储位置

    dentry_info();
};

struct hscfs_dentry_ptr;

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
     * 
     * 若目录项不存在，则返回的info中，ino为INVALID_NID，若当前目录文件还能找到存储该目录项的位置，
     * 则info的store_pos保存可创建位置
     * 
     * 调用者需持有fs_meta_lock
     */
    dentry_info lookup(const std::string &name);

private:
    uint32_t ino;  // 目录文件的ino
    dentry_handle dentry;  // 对应的dentry
    file_system_manager *fs_manager;

private:
    /* 在块中查找目录项 */
    static block_buffer create_formatted_data_block_buffer();

    /* lookup 辅助函数 */
    dentry_info find_dentry_in_block(uint32_t blkno, const std::string &name, u32 name_hash) const;
    static uint32_t bucket_num(u32 level, int dir_level);  // 计算第level级哈希表中桶的个数
    static u32 bucket_block_num(u32 level);  // 计算第level级哈希表的每个桶所包含的block个数
    /* 计算第level级哈希表中，下标为idx的桶的第一个block在目录文件中的块偏移 */
    static u32 bucket_start_block_index(u32 level, int dir_level, u32 bucket_idx);
    static u32 hscfs_dentry_hash(const char* name, u32 len);  // 计算name存储在目录中的hash值
    /* 创建在块中查找的上下文hscfs_dentry_ptr */
    static void make_dentry_ptr_block(struct hscfs_dentry_ptr *d, struct hscfs_dentry_block *t);
    static hscfs_dir_entry *hscfs_find_target_dentry(const unsigned char *name, u32 nameLen, uint32_t namehash, 
        int *max_slots, u32 *empty_bit_pos, hscfs_dentry_ptr *d);
    static u32 hscfs_match_name(struct hscfs_dentry_ptr* d, struct hscfs_dir_entry *de, const unsigned char* name,
        u32 len, unsigned long bit_pos, hscfs_hash_t namehash);
};

}  // namespace hscfs