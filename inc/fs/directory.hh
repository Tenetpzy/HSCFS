#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include "cache/dentry_cache.hh"
#include "cache/block_buffer.hh"
#include "cache/dir_data_block_cache.hh"

namespace hscfs {

class file_system_manager;

/* 保存一个目录项的ino、type和位置信息，内部使用 */
struct dentry_info
{
    uint32_t ino;  // inode号，无效则为INVALID_NID
    uint8_t type;  // 类型
    dentry_store_pos store_pos;  // 存储位置

    dentry_info();
};

struct hscfs_dentry_ptr;

/* 目录操作类，使用此类时，调用者需持有fs_meta_lock */
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
     * 创建新文件的目录项，标记为dirty，将其加入dentry cache，并将其返回
     * 更新当前目录inode中i_dentry_num和访问时间等字段
     * 调用者可以提供创建目录项的位置信息(create_pos_hint)
     * 
     * 不应该在已经存在同名目录项name时调用create，否则结果未定义
     */
    dentry_handle create(const std::string &name, uint8_t type, const dentry_store_pos *create_pos_hint = nullptr);

    /*
     * 创建一个硬链接目录项，目录项名为name，链接到link_ino
     * 调用者保证link_ino有效
     * 与create的区别是，此处不创建新文件，不分配inode
     * 此方法不增加link_ino的nlink计数
     */
    void link(const std::string &name, uint32_t link_ino, const dentry_store_pos *create_pos_hint = nullptr);

    /*
     * 在目录文件中查找目录项name，主机侧完成
     * 
     * 若目录项不存在，则返回的info中，ino为INVALID_NID，若当前目录文件还能找到存储该目录项的位置，
     * 则info的store_pos保存可创建位置
     */
    dentry_info lookup(const std::string &name);

    /*
     * 在目录文件中删除目录项dentry，内部会将修改过的数据和元数据标记dirty
     * 调用者应保证该目录项存在
     */
    void remove(dentry_handle &dentry);

public:
    static uint32_t bucket_num(u32 level, int dir_level);  // 计算第level级哈希表中桶的个数

    static u32 bucket_block_num(u32 level);  // 计算第level级哈希表的每个桶所包含的block个数

private:
    uint32_t ino;  // 目录文件的ino
    dentry_handle dentry;  // 对应的dentry
    file_system_manager *fs_manager;

private:
    static block_buffer create_formatted_data_block_buffer();  // 新建一个格式化后的dir data block buffer
    
    /* 目录项查找辅助函数 */
    /*****************************************************************/

    /* 在块中查找目录项 */
    dentry_info find_dentry_in_block(uint32_t blkno, const std::string &name, u32 name_hash) const;

    /* 计算第level级哈希表中，下标为idx的桶的第一个block在目录文件中的块偏移 */
    static u32 bucket_start_block_index(u32 level, int dir_level, u32 bucket_idx);

    static u32 hscfs_dentry_hash(const char* name, u32 len);  // 计算name存储在目录中的hash值

    /* 创建在块中查找的上下文hscfs_dentry_ptr */
    static void make_dentry_ptr_block(struct hscfs_dentry_ptr *d, struct hscfs_dentry_block *t);

    /* 查看位图中slot_pos是否被占用。被占用返回true。位图起始地址为bitmap_start_addr */
    static bool test_bitmap_pos(unsigned long slot_pos, const void *bitmap_start_addr);

    static hscfs_dir_entry *hscfs_find_target_dentry(const unsigned char *name, u32 nameLen, uint32_t namehash, 
        int *max_slots, u32 *empty_bit_pos, hscfs_dentry_ptr *d);

    static u32 hscfs_match_name(struct hscfs_dentry_ptr* d, struct hscfs_dir_entry *de, const unsigned char* name,
        u32 len, unsigned long bit_pos, hscfs_hash_t namehash);

    /*****************************************************************/

    /* create辅助函数 */
    /*****************************************************************/

    /*
     * 查找目录项name的创建位置。返回一个数据块handle(可在该块中创建)和创建位置
     * 优先使用create_pos_hint
     * 当前目录中找不到可创建位置时，则新建一个哈希表
     */
    std::pair<dir_data_block_handle, dentry_store_pos> get_create_pos(const std::string &name, 
        const dentry_store_pos *create_pos_hint);

    /*
     * 创建目录项，添加目录项到dentry cache，更新inode中相关的元数据，将修改过的缓存标记dirty
     * 返回新创建目录项的handle
     * 调用者保证blk_handle和pos指向正确的创建位置
     */
    dentry_handle create_dentry(const std::string &name, uint8_t type, uint32_t new_inode, 
        dir_data_block_handle &create_blk_handle, const dentry_store_pos &create_pos);

    /* 
     * 检查create_pos_hint指向的位置是否确实能创建目录项name 
     * 如果create_pos_hint超过当前文件最大块偏移max_blk_off，返回false
     * 否则，该位置被其它目录项占用时返回false，未占用或是文件空洞则返回true
     */
    bool is_create_pos_valid(const std::string &name, const dentry_store_pos *create_pos_hint, uint32_t max_blk_off);

    /* 增加1级哈希表 */
    void append_hash_level(hscfs_inode *inode);

    /* 将位图中slot_pos置为1 */
    static void set_bitmap_pos(unsigned long slot_pos, void *bitmap_start_addr);

    /* 在块中指定位置写入目录项信息 */
    void create_dentry_in_blk(const std::string &name, uint8_t type, uint32_t new_ino, 
        dir_data_block_handle blk_handle, const dentry_store_pos &pos);
    
    /*****************************************************************/

    /* remove辅助函数 */

    /* 将位图中slot_pos置为0 */
    static void reset_bitmap_pos(unsigned long slot_pos, void *bitmap_start_addr);

    /* 在块blk_handle中移除目录项dentry，pos为dentry的位置。调用者保证pos正确 */
    void remove_dentry_in_blk(const dentry_handle &dentry, dir_data_block_handle &blk_handle, 
        const dentry_store_pos &pos);
};

}  // namespace hscfs