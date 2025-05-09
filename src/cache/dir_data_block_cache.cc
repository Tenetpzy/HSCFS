#include "cache/dir_data_block_cache.hh"
#include "utils/hscfs_log.h"
#include "utils/hscfs_exceptions.hh"
#include "fs/fs_manager.hh"
#include "fs/file_utils.hh"
#include "fs/fs.h"

namespace hscfs {

dir_data_block_cache::~dir_data_block_cache()
{
    if (!dirty_blks.empty())
        HSCFS_LOG(HSCFS_LOG_WARNING, "dir data block cache still has dirty block while destructed.");
}

void dir_data_block_cache::do_relpace()
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

void dir_data_block_handle::mark_dirty()
{
    cache->mark_dirty(*this);
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

dir_data_block_entry::dir_data_block_entry(uint32_t ino, uint32_t blkoff, uint32_t lpa, block_buffer &&block) noexcept
    : key(ino, blkoff), block(std::move(block))
{
    this->lpa = lpa;
    state = dir_data_block_entry_state::uptodate;
    ref_count = 0;
}

dir_data_block_entry::~dir_data_block_entry()
{
    if (ref_count != 0)
        HSCFS_LOG(HSCFS_LOG_WARNING, "dir data block has non-zero refcount while destructed.");
    if (state == dir_data_block_entry_state::dirty)
        HSCFS_LOG(HSCFS_LOG_WARNING, "dir data block is still dirty while destructed.");
}

std::pair<dir_data_block_handle, block_addr_info> dir_data_cache_helper::get_dir_data_block(uint32_t dir_ino, 
    uint32_t blkno)
{
    dir_data_block_cache *dir_data_cache = fs_manager->get_dir_data_cache();
    dir_data_block_handle handle = dir_data_cache->get(dir_ino, blkno);
    block_addr_info addr;

    /* dir data block缓存不命中，通过file mapping查找地址并读取 */
    if (handle.is_empty())
    {
        file_mapping_util searcher(fs_manager);
        addr = searcher.get_addr_of_block(dir_ino, blkno);

        /* 如果是文件空洞，直接返回 */
        if (addr.lpa == INVALID_LPA)
            return std::make_pair(dir_data_block_handle(), addr);

        block_buffer buffer;
        try
        {
            buffer.read_from_lpa(fs_manager->get_device(), addr.lpa);
        }
        catch (const io_error &e)
        {
            HSCFS_LOG(HSCFS_LOG_ERROR, "dir data block helper: read lpa %u failed.", addr.lpa);
            throw;
        }
        handle = dir_data_cache->add(dir_ino, blkno, addr.lpa, std::move(buffer));
    }

    return std::make_pair(handle, addr);
}

} // namespace hscfs