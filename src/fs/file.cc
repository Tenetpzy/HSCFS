#include <system_error>

#include "cache/page_cache.hh"
#include "fs/file.hh"
#include "fs/fs_manager.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"
#include "file.hh"

namespace hscfs {

file::file(uint32_t ino, file_system_manager *fs_manager)
{
    this->ino = ino;
    this->fs_manager = fs_manager;
    int ret = spin_init(&file_meta_lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "file object: init file meta lock failed.");
    ret = rwlock_init(&file_op_lock);
    if (ret != 0)
    {
        spin_destroy(&file_meta_lock);
        throw std::system_error(std::error_code(ret, std::generic_category()), 
            "file object: init file op lock failed.");
    }
    page_cache_.reset(new page_cache(fs_manager->get_page_cache_size()));
    ref_count = 0;
}

file::~file()
{
    if (is_dirty)
        HSCFS_LOG(HSCFS_LOG_WARNING, "file object is still dirty while destructed.");
    if (ref_count)
        HSCFS_LOG(HSCFS_LOG_WARNING, "file object has non-zero refcount which equals %u while destructed.", 
            ref_count);
}

file_handle file_obj_cache::add(uint32_t ino)
{
    auto p_entry = std::make_unique<file>(ino, fs_manager);
    assert(cache_manager.get(ino) == nullptr);
    file *raw_p = p_entry.get();
    cache_manager.add(ino, p_entry);
    ++cur_size;
    add_refcount(raw_p);
    do_relpace();
    return file_handle(raw_p, this);
}

file_handle file_obj_cache::get(uint32_t ino)
{
    auto p_entry = cache_manager.get(ino);
    if (p_entry != nullptr)
        add_refcount(p_entry);
    return file_handle(p_entry, this);
}

void file_obj_cache::add_refcount(file *entry)
{
    ++entry->ref_count;
    if (entry->ref_count == 1)
        cache_manager.pin(entry->ino);
}

void file_obj_cache::sub_refcount(file *entry)
{
    --entry->ref_count;
    if (entry->ref_count == 0)
        cache_manager.unpin(entry->ino);
}

void file_obj_cache::do_relpace()
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
                HSCFS_LOG(HSCFS_LOG_INFO, "replace file object, inode = %u",
                    p->ino);
            }
            if (p == nullptr || cur_size <= expect_size)
                break;
        }
    }
}

file_handle::~file_handle()
{
    if (entry != nullptr)
    {
        try
        {
            cache->sub_refcount(entry);
        }
        catch(const std::exception &e)
        {
            HSCFS_LOG(HSCFS_LOG_WARNING, "exception during sub_refcount of file object: "
                "%s", e.what());
        }
    }
}

void file_handle::do_addref()
{
    if (entry != nullptr)
        cache->add_refcount(entry);
}

void file_handle::do_subref()
{
    if (entry != nullptr)
        cache->sub_refcount(entry);
}

} // namespace hscfs