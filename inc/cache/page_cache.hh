#pragma once

#include "cache/block_buffer.hh"
#include "cache/cache_manager.hh"
#include "utils/hscfs_multithread.h"
#include "utils/declare_utils.hh"
#include <mutex>
#include <condition_variable>
#include <map>
#include <atomic>

namespace hscfs {

enum class page_state
{
    /* 
     * 缓存块内容无效(未从SSD读或未初始化。未初始化包括文件空洞、文件增加部分等)
     * 处于invalid状态时，origin_lpa和commit_lpa是随机值
     */
    invalid,
    ready  // 缓存块内容有效
};

class page_entry
{
public:
    page_entry(uint32_t blkoff);
    ~page_entry();

    std::mutex& get_page_lock() noexcept
    {
        return page_lock;
    }

    block_buffer& get_page_buffer() noexcept
    {
        return page;
    }

    page_state get_state() const noexcept
    {
        return content_state;
    }

    void set_state(page_state sta) noexcept
    {
        content_state = sta;
    }

    uint32_t get_blkoff() const noexcept
    {
        return blkoff;
    }

    uint32_t get_origin_lpa() const noexcept
    {
        return origin_lpa;
    }

    void set_origin_lpa(uint32_t origin_lpa) noexcept
    {
        this->origin_lpa = origin_lpa;
    }

    uint32_t get_commit_lpa() const noexcept
    {
        return commit_lpa;
    }

    void set_commit_lpa(uint32_t commit_lpa) noexcept
    {
        this->commit_lpa = commit_lpa;
    }

private:

    /* 对is_dirty进行CAS操作(由false更改为true)，成功返回true */
    bool mark_dirty()
    {
        bool expect = false;
        return is_dirty.compare_exchange_strong(expect, true);
    }

private:
    uint32_t blkoff;   // 文件内块偏移
    uint32_t origin_lpa, commit_lpa;  // 旧lpa（如果之前存在）和提交时的新lpa

    page_state content_state;
    block_buffer page;

    /*
     * 保护page、content_state、origin_lpa的锁
     * 获得file_op_lock独占时，不需要再加此锁
     */
    std::mutex page_lock;

    /*
     * 引用计数，在调用方与page_entry_handle生命周期绑定，一个page_entry_handle增加1引用计数
     * page cache内的dirty page set中的page entry也增加引用计数
     * 
     * 通过page_cache.get获取page_entry_handle时，对page_cache加cache_lock锁后增加ref_count
     * ref_count为0时，一定是page cache内部独占访问page_entry(内部加了cache_lock锁，外部没有句柄，无法访问)
     * 
     * page_entry_handle拷贝时，对ref_count原子加，不对page_cache加锁(此时ref_count一定大于等于1)
     * page_entry_handle析构时，调用page_cache的sub_refcount方法
     */
    std::atomic_uint32_t ref_count;

    /* dirty标记 */
    std::atomic_bool is_dirty;

    friend class page_cache;
    friend class page_entry_handle;
};

class page_cache;

/* 外部操作page_entry的句柄 */
class page_entry_handle
{
public:
    page_entry_handle(page_entry *entry, page_cache *cache) noexcept
    {
        this->cache = cache;
        this->entry = entry;
    }
    page_entry_handle(const page_entry_handle &o);
    page_entry_handle(page_entry_handle &&o) noexcept
        : page_entry_handle(o.entry, o.cache)
    {
        o.entry = nullptr;
    }
    page_entry_handle& operator=(const page_entry_handle &o);
    page_entry_handle& operator=(page_entry_handle &&o);

    ~page_entry_handle();

    page_entry* operator->()
    {
        return entry;
    }

    /* 尝试将page entry的dirty置位。如果是由本线程将dirty置位，则加入page cache的dirty pages集合 */
    void mark_dirty();

private:
    page_cache *cache;
    page_entry *entry;

    void do_addref();
    void do_subref();

    friend class page_cache;
};

/* 
 * 文件页缓存
 * page_cache只作为page的缓存索引和置换管理器，不关心文件的实际大小
 */
class page_cache
{
public:
    page_cache(size_t expect_size);
    ~page_cache();

    /*
     * 获取blkoff对应的page_entry
     *
     * 若不存在，则构造一个page_entry，返回其handle
     * 此时返回的page_entry中，反向映射和4KB缓存是无效值
     */
    page_entry_handle get(uint32_t blkoff);

    /*
     * 截断文件后调用，将page cache内块偏移严格大于max_blkoff的缓存page的dirty位清除，置位invalid状态，但
     * 不将它们删除，因为也许之后又会访问
     * 调用者必须持有对应文件的file_op_lock独占锁（此时仅有一个线程能操作文件的page cache）
     */
    void truncate(uint32_t max_blkoff);

private:
    generic_cache_manager<uint32_t, page_entry> cache_manager;
    spinlock_t cache_lock;  // 保护cache_manager

    /* 由于dirty_pages有范围remove需求，所以用map<blkoff, page_handle>维护 */
    std::map<uint32_t, page_entry_handle> dirty_pages;
    spinlock_t dirty_pages_lock;

    size_t expect_size, cur_size;

    /* 调用者需要加cache_lock，除非能够保证调用时ref_count不会为0 */
    void add_refcount(page_entry *entry);

    void sub_refcount(page_entry *entry);
    void do_replace();

    /* 
     * 由page_entry_handle的mark_dirty方法调用
     * 调用时能保证ref_count不为0，因为发起调用的page_entry_handle仍有效，所以内部不加cache_lock锁
     */
    void add_to_dirty_pages(page_entry_handle &page);

    friend class page_entry_handle;
};

}  // namespace hscfs