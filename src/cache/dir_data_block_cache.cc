#include "cache/dir_data_block_cache.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

dir_data_block_cache::~dir_data_block_cache()
{
    if (!dirty_list.empty())
        HSCFS_LOG(HSCFS_LOG_WARNING, "dir data block cache still has dirty block while destructed.");
}

void hscfs::dir_data_block_cache::do_relpace()
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
                HSCFS_LOG(HSCFS_LOG_INFO, "replace dir data block cache entry, inode = %u, blkoff = %u",
                    p->key.ino, p->key.blkoff);
            }
            if (p == nullptr || cur_size <= expect_size)
                break;
        }
    }
}

dir_data_block_handle::~dir_data_block_handle()
{
    if (entry != nullptr)
    {
        try
        {
            cache->sub_refcount(entry);
        }
        catch(const std::exception& e)
        {
            HSCFS_LOG(HSCFS_LOG_WARNING, "exception during sub_refcount of dir data block cache entry: "
                "%s", e.what());
        }
    }
}

void dir_data_block_handle::do_addref()
{
    if (entry != nullptr)
        cache->add_refcount(entry);
}

void dir_data_block_handle::do_subref()
{
    if (entry != nullptr)
        cache->sub_refcount(entry);
}

dir_data_block_entry::~dir_data_block_entry()
{
    if (ref_count != 0)
        HSCFS_LOG(HSCFS_LOG_WARNING, "dir data block has non-zero refcount while destructed.");
    if (state == dir_data_block_entry_state::dirty)
        HSCFS_LOG(HSCFS_LOG_WARNING, "dir data block is still dirty while destructed.");
}

} // namespace hscfs