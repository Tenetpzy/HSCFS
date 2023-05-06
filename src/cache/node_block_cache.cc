#include "cache/node_block_cache.hh"
#include "cache/SIT_NAT_cache.hh"
#include "cache/super_cache.hh"
#include "fs/NAT_utils.hh"
#include "fs/SIT_utils.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "fs/super_manager.hh"
#include "utils/hscfs_log.h"
#include "utils/hscfs_exceptions.hh"

namespace hscfs {

node_block_cache::~node_block_cache()
{
    if (!dirty_list.empty())
        HSCFS_LOG(HSCFS_LOG_WARNING, "node block cache still has dirty block while destructed.");
}

void node_block_cache::sub_refcount(node_block_cache_entry *entry)
{
    --entry->ref_count;
    if (entry->ref_count == 0)
    {
        cache_manager.unpin(entry->nid);

        /* 如果该node block需要删除，则减少其父结点的引用计数，释放它的FS资源，把它移除缓存 */
        if (entry->state == node_block_cache_entry_state::deleted)
        {
            /* 释放它的nid */
            super_manager(fs_manager).free_nid(entry->nid);

            /* 将它占有的lpa标记为垃圾块 */
            uint32_t cur_lpa = INVALID_LPA;
            if (entry->new_lpa != INVALID_LPA)  // 有新地址，则标记新地址，旧地址(如果有)应当在写入时标记过了
                cur_lpa = entry->new_lpa;
            else if (entry->old_lpa != INVALID_LPA)  // 否则，如果有旧地址（说明不是新创建且未写入的），则标记旧地址
                cur_lpa = entry->old_lpa;
            if (cur_lpa != INVALID_LPA)
            {
                HSCFS_LOG(HSCFS_LOG_INFO, "the lpa of nid [%u] is [%u], which will be invalidated.", 
                    entry->nid, cur_lpa);
                SIT_operator(fs_manager).invalidate_lpa(cur_lpa);
            }

            /* 减少父结点的引用计数 */
            uint32_t parent_nid = entry->parent_nid;
            if (parent_nid != INVALID_NID)
            {
                auto parent = cache_manager.get(parent_nid, false);
                assert(parent != nullptr);
                sub_refcount(parent);
            }

            cache_manager.remove(entry->nid);
        }
    }
}

void node_block_cache::do_replace()
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
                HSCFS_LOG(HSCFS_LOG_INFO, "replace node block cache entry, nid = %u", p->nid);

                // 将parent的引用计数-1
                uint32_t parent_nid = p->parent_nid;
                if (parent_nid != INVALID_NID)
                {
                    auto parent = cache_manager.get(parent_nid, false);
                    assert(parent != nullptr);
                    sub_refcount(parent);
                }
            }
            if (p == nullptr || cur_size <= expect_size)
                break;
        }
    }
}

node_block_cache_entry_handle::~node_block_cache_entry_handle()
{
    if (entry != nullptr)
    {
        try
        {
            cache->sub_refcount(entry);
        }
        catch (const std::exception &e)
        {
            HSCFS_LOG(HSCFS_LOG_WARNING, "exception during sub_refcount of node block cache entry: "
                "%s", e.what());
        }
    }
}

void node_block_cache_entry_handle::add_host_version()
{
    cache->add_refcount(entry);
}

void node_block_cache_entry_handle::add_SSD_version()
{
    cache->sub_refcount(entry);
}

void node_block_cache_entry_handle::mark_dirty() const noexcept
{
    cache->mark_dirty(*this);
}

void node_block_cache_entry_handle::delete_node()
{
    cache->remove_entry(entry);
}

void node_block_cache_entry_handle::do_addref()
{
    if (entry != nullptr)
        cache->add_refcount(entry);
}

void node_block_cache_entry_handle::do_subref()
{
    if (entry != nullptr)
        cache->sub_refcount(entry);
}

node_block_cache_entry::~node_block_cache_entry()
{
    if (ref_count != 0 || state != node_block_cache_entry_state::uptodate)
    {
        HSCFS_LOG(HSCFS_LOG_WARNING, "node block cache entry has non-zero refcount or is in dirty state when destructed, "
            "refcount = %u, state = %s", ref_count, 
            state == node_block_cache_entry_state::uptodate ? "uptodate" : "dirty");
    }
}

node_cache_helper::node_cache_helper(file_system_manager *fs_manager) noexcept
{
    dev = fs_manager->get_device();
    nat_cache = fs_manager->get_nat_cache();
    node_cache = fs_manager->get_node_cache();
    this->fs_manager = fs_manager;
}

node_block_cache_entry_handle node_cache_helper::get_node_entry(uint32_t nid, uint32_t parent_nid)
{
    node_block_cache_entry_handle node_handle = node_cache->get(nid);
    if (node_handle.is_empty())
    {
        /* 从NAT表中得到nid block的lpa */
        uint32_t nid_lpa = nat_lpa_mapping(fs_manager).get_lpa_of_nid(nid);

        block_buffer buf;
        try {
            buf.read_from_lpa(dev, nid_lpa);
        }
        catch (const io_error &e) {
            HSCFS_LOG(HSCFS_LOG_ERROR, "node cache helper: read lpa %u failed.", nid_lpa);
            throw;
        }
        node_handle = node_cache->add(std::move(buf), nid, parent_nid, nid_lpa);
    }
    
    hscfs_node *node = node_handle->get_node_block_ptr();
    assert(node->footer.nid == nid);
    if (parent_nid == INVALID_NID)
        assert(node->footer.ino == nid);

    return node_handle;
}

node_block_cache_entry_handle node_cache_helper::create_node_entry(uint32_t ino, uint32_t noffset, uint32_t parent_nid)
{
    /* 分配nid，创建node block缓存项并加入缓存 */
    uint32_t new_nid = super_manager(fs_manager).alloc_nid(ino);
    auto handle = node_cache->add(block_buffer(), new_nid, parent_nid, INVALID_LPA);
    hscfs_node *node = handle->get_node_block_ptr();

    /* 初始化node footer */
    node_footer *footer = &node->footer;
    footer->ino = ino;
    footer->nid = new_nid;
    footer->offset = noffset;

    /* 标记缓存项为dirty */
    handle.mark_dirty();

    return handle;
}

} // namespace hscfs