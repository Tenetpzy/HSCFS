#pragma once

#include <cstdint>
#include <ctime>
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
 * file mapping工具
 * 使用此类前需要持有fs_meta_lock
 */
class file_mapping_util
{
public:
    file_mapping_util(file_system_manager *fs_manager)
    {
        this->fs_manager = fs_manager;
    }

    /* 
     * 得到文件ino中，块号blkno的地址信息
     * 调用者需保证ino和blkno合法 
     */
    block_addr_info get_addr_of_block(uint32_t ino, uint32_t blkno);

    /* 
     * file mapping查询的辅助函数

     * block：带查找的块号
     * 
     * offset: block在索引树路径的每一级索引块中的偏移：
     * 对于inode，若block在直接索引范围内，offset是i_attr数组下标；否则offset - NODE_DIR1_BLOCK是i_nid数组下标
     * 对于indirect和direct node，offset是nid/addr数组下标
     * 
     * noffset：索引树路径的每一个块的树上逻辑编号
     * 
     * 返回值：block的索引树路径深度。inode深度为0。
     */
    static int get_node_path(uint64_t block, uint32_t offset[4], uint32_t noffset[4]);

    /*
     * 封装从node和offset获取下一级nid的过程
     * 若node是inode，则cur_level应为0，否则应为非0
     * node不能是direct node
     */
    static uint32_t get_next_nid(hscfs_node *node, uint32_t offset, int cur_level);

    /*
     * 封装：设置node中offset对应下一级nid为nxt_nid的过程
     * 若node是inode，则cur_level应为0，否则应为非0。node不能是direct node
     * 调用者应把node对应缓存项标记为dirty
     */
    static void set_next_nid(hscfs_node *node, uint32_t offset, int cur_level, uint32_t nxt_nid);

    /*
     * 封装：从inode或direct node和offset中获取lpa的过程
     * 若node是inode，则level应为0，否则应为非0
     * node不能是indirect node
     */
    static uint32_t get_lpa(hscfs_node *node, uint32_t offset, int level);

    /*
     * 封装：设置inode或direct node和offset对应的索引项为lpa的过程
     * 若node是inode，则level应为0，否则应为非0。node不能是indirect node
     * 调用者应把node对应缓存项标记为dirty
     */
    static void set_lpa(hscfs_node *node, uint32_t offset, int level, uint32_t lpa);
    
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
     * 将ino减小至tar_size字节，如果当前文件大小<=tar_size，什么都不做
     * 
     * 将指向受影响块范围的direct pointer全部置为INVALID_LPA，并删除不再索引任何block的node。
     * 
     * 对于受影响的文件块范围，将invalidate该块的lpa。即，若块缓存中有受到影响的块，
     * 调用者只需要把它们从数据块缓存中移除，不应再无效化该块的lpa。
     * 这么做的原因是：
     * 1、无法保证所有块都在缓存中，如果不由reduce方法invalidate，则一些lpa无法标记为垃圾块
     *  （reduce后无法再做file mapping，找不到该块的lpa了）
     * 2、目前由SIT_operator完成invalidate操作，目前为了方便定位潜在的BUG，SIT_operator不允许同一位置的二次validate/invalidate
     */
    void reduce(uint32_t ino, uint64_t tar_size);

    /* 
     * 将ino扩大至tar_size字节。
     * 如果当前文件大小>=tar_size，什么都不做。
     * 扩大的部分不会分配物理块，置为INVALID_LPA。
     */
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

/* 文件创建器 */
class file_creator
{
public:
    file_creator(file_system_manager *fs_manager)
    {
        this->fs_manager = fs_manager;
    }

    /* 
     * 创建普通文件，返回其inode的handle，内部标记inode为dirty
     * 目前的实现中，不使用内联文件
     */
    node_block_cache_entry_handle create_generic_file();

    /*
     * 创建目录文件，返回其inode的handle，内部标记inode为dirty
     * 目前实现中不使用内联目录
     */
    node_block_cache_entry_handle create_directory();

private:
    file_system_manager *fs_manager;

    /* 分配inode，初始化其中的公共元数据。内部会标记inode缓存项为dirty */
    node_block_cache_entry_handle create_base_inode();
};

/* inode时间设置工具 */
class inode_time_util
{
public:
    inode_time_util(file_system_manager *fs_manager)
    {
        this->fs_manager = fs_manager;
    }

    /* 
     * 设置inode中，i_atime和i_atime_nsec为time参数。若time为空则置为当前时间
     * 需要调用者标记dirty
     */
    static void set_atime(hscfs_inode *inode, const timespec *time = nullptr);

    /* 设置inode中mtime，其余与set_atime相同 */
    static void set_mtime(hscfs_inode *inode, const timespec *time = nullptr);

    /* 
     * 标记文件被访问，更新atime为当前时间
     * 此方法内部标记inode为dirty
     */
    void mark_access(uint32_t ino);

    /*
     * 标记文件被修改，更新atime和mtime为当前时间
     * 此方法内部标记inode为dirty
     */
    void mark_modified(uint32_t ino);
    
private:
    file_system_manager *fs_manager;
};

}  // namespace hscfs