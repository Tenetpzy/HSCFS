#pragma once

#include <memory>
#include <ctime>
#include <atomic>
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

private:
    uint32_t ino;  // inode号
    file_system_manager *fs_manager;

    uint64_t size;  // 当前文件大小（字节数）
    timespec atime;  // 访问时间戳
    timespec mtime;  // 修改时间戳（ctime == mtime，不设ctime字段）
    
    /* 保护size、atime、mtime的锁，这些字段会在page cache层并发访问 */
    spinlock_t file_meta_lock;
    
    /* 如果file中的元数据被修改，或page cache有脏页，则is_dirty置为true，同时加入dirty set */
    std::atomic_bool is_dirty;

    /*
     * 硬连接数，访问时加fs_meta_lock
     * close时，应当检查该字段。如果为0，且fd_ref_count也为0，需要删除文件，修改目录项的状态为已删除且未引用 
     */
    uint32_t nlink;

    /* 
     * file_handle引用计数，用于确保file对象不被置换
     * file_handle构造和析构时可能不会持有任何锁，此处使用atomic变量
     */
    std::atomic_uint32_t ref_count;

    /*
     * 目前有多少个fd引用。值为ref_count的一部分，
     * 需要单独设立此字段，原因是无法从ref_count判断file是否被fd引用（可能在dirty set中，但已经close了）
     * 访问时需要加fs_meta_lock
     */
    uint32_t fd_ref_count;

    /* 当前文件对应的目录项 */
    dentry_handle dentry;

    /* 
     * 对文件操作的锁。对file进行任何操作前，获取该锁的共享/独占
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

    /* 标记文件被访问/修改。更新file对象内的atime和mtime为当前时间，不修改is_dirty。 */
    void mark_access();
    void mark_modified();

    friend class file_obj_cache;
    friend class file_cache_helper;
    friend class file_handle;
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

    void mark_dirty();

    void read_meta()
    {
        entry->read_meta();
    }

    /*
     * 在创建opened_file结构时调用
     * 增加file和其关联的dentry的fd引用计数
     * 调用者需持有fs_meta_lock
     */
    void add_fd_refcount();

    /*
     * opened_file结构销毁时由系统调用
     * 增加file和其关联的dentry的fd引用计数
     * 调用者需持有fs_meta_lock
     */
    void sub_fd_refcount();

    rwlock_t& get_file_op_lock() noexcept
    {
        return entry->file_op_lock;
    }

    /*
     * 调整文件大小到tar_size
     * 不调整文件page cache中多余的部分。该部分应在write和write back时特殊处理
     * 调用者应持有fs_meta_lock和file_op_lock独占
     */
    int truncate(size_t tar_size);

private:
    file *entry;
    file_obj_cache *cache;

    void do_addref();
    void do_subref();

    friend class file_obj_cache;
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

    /* add和get的调用者需要获取fs_meta_lock */
    file_handle add(uint32_t ino, const dentry_handle &dentry);
    file_handle get(uint32_t ino);

private:
    size_t expect_size, cur_size;
    file_system_manager *fs_manager;

    /* 
     * 目前，cache_manager可以由fs_meta_lock锁及fs_freeze_lock独占锁保护，不需要额外的锁
     * 分析见其它访问cache_manager的方法的注释
     */
    generic_cache_manager<uint32_t, file> cache_manager;

    /* 
     * 脏文件集合，与保护这个集合的锁 
     * 为了避免在page cache的并发读写标记dirty时，需要获取fs_meta_lock，
     * 脏文件集合使用自己的锁保护
     */
    std::unordered_map<uint32_t, file_handle> dirty_files;
    spinlock_t dirty_files_lock;
    
    /* 在调用add、get或handle拷贝构造(加入dirty set)时调用，是安全的，
     * 因为add、get访问cache_manager会加fs_meta_lock锁。handle拷贝时引用计数不为0，不会访问cache_manager
     */
    void add_refcount(file *entry);

    /* 
     * 在handle析构时调用。必须保证析构时持有fs_meta_lock锁
     * 析构的场景：1) 应用程序调用close. 2) 从dirty set中移除
     * 以上两个场景都会加fs_meta_lock锁（或更高层次的锁）
     */
    void sub_refcount(file *entry);

    /* 仅在add内调用，获取了fs_meta_lock锁 */
    void do_relpace();

    /* 由file handle调用，将file加入dirty_files集合 */
    void add_to_dirty_files(const file_handle &file);

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
