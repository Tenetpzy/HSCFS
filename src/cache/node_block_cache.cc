#include "cache/node_block_cache.hh"
#include "cache/SIT_NAT_cache.hh"
#include "cache/super_cache.hh"
#include "fs/NAT_utils.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"
#include "utils/hscfs_log.h"
#include "utils/hscfs_exceptions.hh"

namespace hscfs {

node_block_cache::~node_block_cache()
{
    if (!dirty_list.empty())
        HSCFS_LOG(HSCFS_LOG_WARNING, "node block cache still has dirty block while destructed.");
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

hscfs::node_block_cache_entry::~node_block_cache_entry()
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
            HSCFS_LOG(HSCFS_LOG_ERROR, "node block fetcher: read lpa %u failed.", nid_lpa);
            throw;
        }
        node_handle = node_cache->add(std::move(buf), nid, parent_nid, nid_lpa);
    }
    return node_handle;
}

} // namespace hscfs