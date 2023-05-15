#pragma once

#include <memory>
#include <ctime>
#include <atomic>
#include "cache/dentry_cache.hh"
#include "utils/hscfs_multithread.h"

namespace hscfs {

class page_cache;
class page_entry_handle;
class file_system_manager;

/* 
 * 作用类似VFS的inode，代表文件系统中的一个文件
 */
class file
{
public:

    /* 构造后，元数据信息(size、atime、mtime、nlink、is_dirty无效，需要通过read_meta读上来) */
    file(uint32_t ino, const dentry_handle &dentry, file_system_manager *fs_manager);
    ~file();

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

    uint32_t get_fd_refcount() const noexcept
    {
        return fd_ref_count;
    }

    void add_nlink() noexcept
    {
        ++nlink;
    }

    void sub_nlink() noexcept
    {
        --nlink;
    }

    uint32_t get_nlink() const noexcept
    {
        return nlink;
    }

    uint32_t get_inode() const noexcept
    {
        return ino;
    }

    rwlock_t& get_file_op_lock() noexcept
    {
        return file_op_lock;
    }

    /*
     * 调整文件大小到tar_size
     * 不调整文件page cache中多余的部分。该部分应在write和write back时特殊处理
     * 调用者应持有fs_meta_lock和file_op_lock独占
     * 此方法内无法标记dirty，如果返回true, 调用者应在稍后调用handle的mark_dirty
     * 
     * 如果改变了文件大小，返回true。如果没有做任何修改，返回false
     */
    bool truncate(size_t tar_size);

    /*
     * 从pos开始读最多count字节
     * 更新file内的atime(但不更新inode中对应元数据)
     * 调用者应在稍后使用file_handle标记dirty
     * 调用者应持有该文件的pos_lock锁
     */
    ssize_t read(char *buffer, ssize_t count, uint64_t pos);

    /*
     * 从pos开始写入count字节
     * 更新file内的atime和mtime，如果增加了大小，更新size(但不更新inode中对应的元数据)
     * 调用者应在稍后使用file_handle标记dirty
     * 调用者应持有该文件的pos_lock锁
     */
    ssize_t write(char *buffer, ssize_t count, uint64_t pos);

private:
    uint32_t ino;  // inode号
    file_system_manager *fs_manager;

    uint64_t size;  // 当前文件大小（字节数）
    timespec atime;  // 访问时间戳
    timespec mtime;  // 修改时间戳（ctime == mtime，不设ctime字段）
    
    /* 保护size、atime、mtime的锁，这些字段会在page cache层并发访问 */
    spinlock_t file_meta_lock;
    
    /* 
     * 如果file中的元数据被修改，或page cache有脏页，则is_dirty置为true，同时加入dirty set
     * 此标记的修改始终由file_handle进行
     */
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

    /* 对is_dirty进行CAS操作(由false更改为true)，成功返回true。由file_handle中mark_dirty调用 */
    bool mark_dirty();

    /* 
     * 读取元数据到对象中，将is_dirty置位false
     * 调用此方法前，file内元数据和dirty标志都是无效的
     * 调用者需持有fs_meta_lock
     */
    void read_meta();

    /* 
     * 标记文件被访问/修改。更新file对象内的atime和mtime为当前时间，不修改is_dirty。
     * 调用者不需要获取file_meta_lock
     */
    void mark_access();
    void mark_modified();

    /* 
     * read过程调用，得到file的文件大小size字段
     * 调用者不需要获取file_meta_lock
     */
    uint64_t get_cur_size();

    /*
     * write过程调用。如果size_after_write大于file->size，则file->size更新为size_after_write
     * 调用者不需要获取file_meta_lock
     */
    void set_cur_size_if_larger(uint64_t size_after_write);

    /* 获取pos对应的块号 */
    uint32_t idx_of_blk(uint64_t pos)
    {
        return pos / 4096;
    }

    /* 获取pos对应的块内偏移 */
    uint32_t off_in_blk(uint64_t pos)
    {
        return pos % 4096;
    }

    /* 获取cur_pos所在块的尾后文件偏移 */
    uint64_t end_pos_of_cur_blk(uint64_t cur_pos)
    {
        uint64_t res = cur_pos + 4096 - off_in_blk(cur_pos);
        assert(res % 4096 == 0);
        return res;
    }

    /*
     * 准备好一个page的内容，包括缓存，lpa，dirty等字段（要么从SSD读上来，要么初始化一个新页面，并初始化其它信息）
     * 
     * 即使该page在文件空洞中、或超出了文件当前范围，此方法也不会将page标记为dirty
     * 原因：对于read，不标记dirty，则维持文件空洞状态，不必分配SSD block。对于write，则应在write方法内标记为dirty。
     * 但由于前期设计时block_buffer和内存分配耦合，会出现不必要的内存分配（未优化）
     * 
     * 调用者需持有该page的page_lock，不得持有fs_meta_lock
     */
    void prepare_page_content(page_entry_handle &page);

    friend class file_obj_cache;
    friend class file_handle;
    friend class file_cache_helper;
};

class file_obj_cache;
/* 同其它缓存一样，file_handle封装引用计数 */
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

    file* operator->()
    {
        return entry;
    }

    bool is_empty() const noexcept
    {
        return entry == nullptr;
    }

    void mark_dirty();

    /*
     * 删除该文件，释放该文件的全部资源，从file_obj_cache中移除entry
     * 调用此方法后，this无效，is_empty返回true，调用者不应再使用此句柄
     * 将对应dentry状态置为deleted，标记dirty
     * 调用者需持有fs_meta_lock
     */
    void delete_file();

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
    file_obj_cache(size_t expect_size, file_system_manager *fs_manager);

    /* add、get、contains的调用者需要获取fs_meta_lock */
    file_handle add(uint32_t ino, const dentry_handle &dentry);
    file_handle get(uint32_t ino);
    bool contains(uint32_t ino);   // 检查当前file_obj_cache中，是否存在inode为ino的file对象

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
     * 为了避免在page cache的并发读写标记dirty时，需要获取fs_meta_lock，脏文件集合使用自己的锁保护
     * 有删除需求，所以使用unordered_map<inode, file_handle>维护
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

    /* 由file handle调用，将entry从缓存中移除。调用者获取了fs_meta_lock */
    void remove_file(file *entry);

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
