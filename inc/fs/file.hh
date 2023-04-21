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
    file(uint32_t ino, const dentry_handle &dentry, file_system_manager *fs_manager);
    ~file();

    /*
     * 截断文件，只剩前remain_block_num个block
     * 调用者需要持有fs_meta_lock
     */
    void truncate(size_t remain_block_num);
    /* to do */

private:
    uint32_t ino;  // inode号

    uint64_t size;  // 当前文件大小（字节数）
    timespec atime;  // 访问时间戳
    timespec mtime;  // 修改时间戳（ctime == mtime，不设ctime字段）
    bool is_dirty;
    
    /* 保护size、atime、mtime、is_dirty的锁，对于文件的file对象，会被page cache层并发访问 */
    spinlock_t file_meta_lock;
    
    file_system_manager *fs_manager;

    /*
     * 硬连接数，访问时加fs_meta_lock
     * close时，应当检查该字段。如果为0，且ref_count也为0，需要删除文件，修改目录项的状态为已删除且未引用 
     */
    uint32_t nlink;

    /* 
     * 引用计数，访问时加fs_meta_lock
     * 引用计数确保file对象不被置换
     * file为dirty状态时，应把handle加到dirty_list中从而增加引用计数 
     */
    uint32_t ref_count;

    /* 当前文件对应的目录项 */
    dentry_handle dentry;

    /* 
     * 对文件操作的锁。对file进行任何操作前，必须获取该锁的共享/独占
     * 此锁的层级在file_mata_lock上。只在需要修改file中元数据时获取file_meta_lock
     */
    rwlock_t file_op_lock;

    std::unique_ptr<page_cache> page_cache_;

private:

    /* 
     * 读取元数据到对象中，将is_dirty置位false
     * 调用此方法前，file内元数据和dirty标志都是无效的
     * 调用者需持有fs_meta_lock
     */
    void read_meta();

    friend class file_obj_cache;
    friend class file_cache_helper;
};

/*
 * 同其它缓存一样，file_handle封装引用计数
 * 但file_handle对象在构造、拷贝、析构时，程序必须保证获取了fs_meta_lock
 */
class file_obj_cache;
class file_handle
{
public:
    file_handle() noexcept
    {
        entry = nullptr;
        cache = nullptr;
    }

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

    file_handle add(uint32_t ino, const dentry_handle &dentry);
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

class file_cache_helper
{
public:
    file_cache_helper(file_obj_cache *file_cache)
    {
        this->file_cache = file_cache;
    }

    /*
     * 获得inode对应的file对象
     * 参数为file对象的ino，该inode对应的dentry
     * 保证返回的file对象内元数据有效
     * 调用者应确保ino和dentry合法
     * 调用者需持有fs_meta_lock 
     */
    file_handle get_file_obj(uint32_t ino, const dentry_handle &dentry);

private:
    file_obj_cache *file_cache;

};

} // namespace hscfs
