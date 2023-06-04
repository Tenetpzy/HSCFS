#include "cache/dentry_cache.hh"
#include "cache/node_block_cache.hh"
#include "cache/super_cache.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
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

    /* 构造目录项时，默认状态为valid，有需要时调用set方法更改 */
    state = dentry_state::valid;

    /* 构造时默认与SSD同步，如果是新建或删除目录项，应设置state后调用handle的mark_dirty */
    is_dirty = false;
}

uint8_t dentry::get_type()
{
    if (type != HSCFS_FT_UNKNOWN)
        return type;
    
    /* 从inode中获取文件类型 */
    node_cache_helper node_helper(fs_manager);
    auto node_handle = node_helper.get_node_entry(ino, INVALID_NID);
    hscfs_node *node = node_handle->get_node_block_ptr();
    assert(node->footer.ino == node->footer.nid);
    type = node->i.i_type;
    assert(type != HSCFS_FT_UNKNOWN);
    return type;
}

dentry_cache::~dentry_cache()
{
    if (!dirty_list.empty())
        HSCFS_LOG(HSCFS_LOG_WARNING, "dentry cache still has dirty dentry when destructed.");
}

void dentry_cache::do_replace()
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
                if (parent != nullptr)  // p是root的情况下，parent == nullptr(全局析构时将root移除)
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

dentry_store_pos::dentry_store_pos()
{
    is_valid = false;
    blkno = 0;
    slotno = INVALID_DENTRY_BITPOS;
}

} // namespace hscfs