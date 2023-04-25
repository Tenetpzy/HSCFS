#pragma once

#include <cstdint>
#include "cache/node_block_cache.hh"

struct hscfs_node;
struct hscfs_inode;

namespace hscfs {

class file_system_manager;

/*
 * 保存一个block的地址信息
 * 包含它的物理地址lpa，和反向映射
 * 
 * 反向映射中的nid_off：
 * 若是inode中的direct_pointer，则nid_off是i_addr数组下标
 * 否则是direct node，nid_off是addr数组下标
 */
struct block_addr_info
{
    uint32_t lpa;
    uint32_t nid;
    uint32_t nid_off;
    node_block_cache_entry_handle nid_handle;
};

/* 保存一个block在索引树上的路径信息 */
struct block_node_path
{
    int level;
    uint32_t offset[4];
    uint32_t noffset[4];
};

/*
 * file mapping解析的执行器
 * 使用此类前需要持有fs_meta_lock
 */
class file_mapping_searcher
{
public:
    file_mapping_searcher(file_system_manager *fs_manager)
    {
        this->fs_manager = fs_manager;
    }

    /* 
     * 得到文件ino中，块号blkno的地址信息
     * 调用者需保证ino和blkno合法 
     */
    block_addr_info get_addr_of_block(uint32_t ino, uint32_t blkno);

    static int get_node_path(uint64_t block, uint32_t offset[4], uint32_t noffset[4]);
    static uint32_t get_next_nid(hscfs_node *node, uint32_t offset, int cur_level);
    static uint32_t get_lpa(hscfs_node *node, uint32_t offset, int level);
    
private:
    file_system_manager *fs_manager;
};

/* 
 * 文件大小调整器
 * 只调整文件元数据(inode和node)，不处理文件数据的缓存
 * 会将修改过的node标记为dirty
 */
class file_resizer
{
public:
    file_resizer(file_system_manager *fs_manager)
    {
        this->fs_manager = fs_manager;
    }

    /* 
     * 将ino减小至size字节。
     * 
     * 将指向受影响块范围的direct pointer全部置为INVALID_LPA，并删除不再索引任何block的node。
     * 
     * 对于受影响的文件块范围，将invalidate该块的lpa。即，若块缓存中有受到影响的块，
     * 调用者只需要把它们从数据块缓存中移除，不应再无效化该块的lpa。
     * 这么做的原因是：
     * 1、无法保证所有块都在缓存中，如果不由reduce方法invalidate，则一些lpa无法标记为垃圾块
     *  （reduce后无法再做file mapping，找不到该块的lpa了）
     * 2、目前由SIT_operator完成invalidate操作，目前为了方便定位潜在的BUG，SIT_operator不允许同一位置的二次validate/invalidate
     * 
     * 调用者应保证tar_size小于当前文件大小。
     */
    void reduce(uint32_t ino, uint64_t tar_size);

    /* 将ino扩大至size字节。扩大的部分不会分配物理块，置为INVALID_LPA。*/
    void expand(uint32_t ino, uint64_t tar_size);

private:
    file_system_manager *fs_manager;

    /* 
     * single node(一级间接索引块，即包含direct pointer的索引块)
     * double node(二级间接索引块，即包含single node pointer的索引块)
     * triple node(三级间接索引块，即包含double node的索引块)
     * 
     * 分别表示上述三种索引块，一个索引块索引了多少个block
     */
    static const uint32_t single_node_blks, double_node_blks, triple_node_blks;
    static const uint64_t max_blkno_limit;  // 文件支持的最大块号 

private:
    /* 
     * 释放[start_blk, end_blk]与它们的索引node block 
     * 当前的实现中，end_blk应该设置为文件当前的最后一个块号，否则assert失败
     */
    void free_blocks_in_range(hscfs_node *inode, uint32_t start_blk, uint32_t end_blk);
};

}  // namespace hscfs