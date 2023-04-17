#include "cache/dentry_cache.hh"
#include "cache/node_block_cache.hh"
#include "cache/super_cache.hh"
#include "fs/fs_manager.hh"
#include "fs/node_block_fetcher.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

dentry::dentry(uint32_t dir_ino, dentry *parent, uint32_t dentry_ino, const std::string &dentry_name, 
        file_system_manager *fs_manager)
    : key(dir_ino, dentry_name)
{
    ino = dentry_ino;
    this->parent = parent;
    this->fs_manager = fs_manager;
    ref_count = 0;

    /* 由ssd path lookup无法获得文件类型，暂时置为unknown，第一次访问时从inode block中读取并缓存 */
    type = HSCFS_FT_UNKNOWN;

    /* 由ssd path lookup无法获取非叶结点dentry的存储位置，暂时置为INVALID，需要时在目录文件中查找并缓存 */
    is_dentry_pos_valid = false;

    /* 构造目录项时，默认状态为valid，有需要时调用set方法更改 */
    state = dentry_state::valid;

    /* 构造时默认与SSD同步，如果是新建或删除目录项，应设置state后调用handle的mark_dirty */
    is_dirty = false;
}

uint8_t dentry::get_type()
{
    if (type != HSCFS_FT_UNKNOWN)
        return type;
    
    /* 从inode中获取文件类型，首先从node block cache中查找ino对应block */
    node_block_cache *node_cache = fs_manager->get_node_cache();
    node_block_cache_entry_handle node_handle = node_cache->get(ino);

    /* node block cache缓存不命中，从SSD读该node block */
    if (node_handle.is_empty())
    {
        super_cache *super = fs_manager->get_super_cache();
        node_block_fetcher fetcher(fs_manager->get_device(), fs_manager->get_nat_cache(), 
            fs_manager->get_node_cache(), super->nat_blkaddr, super->segment_count_nat);
        
        // 这里获取inode block，所以parent为INVALID_NID
        node_handle = fetcher.get_node_entry(ino, INVALID_NID);
    }

    /* 从inode中获取文件类型 */
    hscfs_node *node = node_handle->get_node_block_ptr();
    assert(node->footer.ino == node->footer.nid);
    type = node->i.i_type;
    assert(type != HSCFS_FT_UNKNOWN);
    return type;
}

void hscfs::dentry_cache::do_replace()
{
    if (cur_size > expect_size)
    {
        while (true)
        {
            auto p = cache_manager.replace_one();
            if (p != nullptr)
            {
                assert(p->ref_count == 0);
                --cur_size;
                HSCFS_LOG(HSCFS_LOG_INFO, "replace dentry, dir inode = %u, name = %s", p->key.dir_ino, 
                    p->key.name.c_str());

                // 将parent的引用计数-1
                dentry *parent = p->parent;
                assert(parent != nullptr);
                sub_refcount(parent);
            }
            if (p == nullptr || cur_size <= expect_size)
                break;
        }
    }
}

void dentry_handle::mark_dirty() const noexcept
{
    cache->mark_dirty(*this);
}

void dentry_handle::do_addref()
{
    if (entry != nullptr)
        cache->add_refcount(entry);
}

void dentry_handle::do_subref()
{
    if (entry != nullptr)
        cache->sub_refcount(entry);
}

} // namespace hscfs