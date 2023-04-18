#pragma once

#include <memory>
#include <ctime>
#include "cache/dentry_cache.hh"
#include "utils/hscfs_multithread.h"

namespace hscfs {

class page_cache;
class file_system_manager;

/* 
 * 作用类似VFS的inode，代表文件系统中的一个文件
 */
class file
{
public:

    /* 构造后，元数据信息(size、atime、mtime、nlink、is_dirty无效，通过read_meta_data读上来) */
    file(uint32_t ino, file_system_manager *fs_manager);
    ~file();
    /* to do */

protected:
    uint32_t ino;  // inode号

    uint64_t size;  // 当前文件大小（字节数）
    timespec atime;  // 访问时间戳
    timespec mtime;  // 修改时间戳（ctime == mtime，不设ctime字段）
    bool is_dirty;
    
    /* 保护size、atime、mtime、is_dirty的锁，对于文件的file对象，会被page cache层并发访问 */
    spinlock_t file_meta_lock;
    
    file_system_manager *fs_manager;

    uint32_t nlink;  // 硬连接数，访问时加fs_meta_lock

    /* 
     * 引用计数，访问时加fs_meta_lock
     * 引用计数确保file对象不被置换
     * file为dirty状态时，应把handle加到dirty_list中从而增加引用计数 
     */
    uint32_t ref_count;

    /* 
     * 对文件操作的锁。对generic_file进行任何操作前，必须获取该锁的共享/独占
     * 此锁的层级在file_mata_lock上。只在需要修改file中元数据时获取file_meta_lock
     */
    rwlock_t file_op_lock;

    std::unique_ptr<page_cache> page_cache_;

    friend class file_obj_cache;
};

/*
 * 同其它缓存一样，file_handle封装引用计数
 * 但file_handle对象在构造、拷贝、析构时，程序必须保证获取了fs_meta_lock
 */
class file_obj_cache;
class file_handle
{
public:
    file_handle(file *entry, file_obj_cache *cache) noexcept
    {
        this->entry = entry;
        this->cache = cache;
    }

    file_handle(const file_handle &o)
    {
        entry = o.entry;
        cache = o.cache;
        do_addref();
    }

    file_handle(file_handle &&o) noexcept
    {
        entry = o.entry;
        cache = o.cache;
        o.entry = nullptr;
    }

    ~file_handle();

    file_handle& operator=(const file_handle &o)
    {
        if (this != &o)
        {
            do_subref();
            entry = o.entry;
            cache = o.cache;
            do_addref();
        }
        return *this;
    }

    file_handle& operator=(file_handle &&o)
    {
        if (this != &o)
        {
            do_subref();
            entry = o.entry;
            cache = o.cache;
            o.entry = nullptr;
        }
        return *this;
    }

    bool is_empty() const noexcept
    {
        return entry == nullptr;
    }

    file* operator->() const noexcept
    {
        return entry;
    }

private:
    file *entry;
    file_obj_cache *cache;

    void do_addref();
    void do_subref();
};

/* file对象的缓存 */
class file_obj_cache
{
public:
    file_obj_cache(size_t expect_size, file_system_manager *fs_manager)
    {
        this->expect_size = expect_size;
        this->fs_manager = fs_manager;
        cur_size = 0;
    }

    file_handle add(uint32_t ino);
    file_handle get(uint32_t ino);

private:
    size_t expect_size, cur_size;
    file_system_manager *fs_manager;
    generic_cache_manager<uint32_t, file> cache_manager;
    
    void add_refcount(file *entry);
    void sub_refcount(file *entry);
    void do_relpace();

    friend class file_handle;
};

} // namespace hscfs
