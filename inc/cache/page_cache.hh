#pragma once

#include "cache/block_buffer.hh"
#include "cache/cache_manager.hh"
#include "utils/hscfs_multithread.h"
#include "utils/declare_utils.hh"
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>

namespace hscfs {

enum class page_state
{
    invalid,  // 缓存还没从SSD读上来
    reading,  // 缓存正在由某个线程从SSD读
    ready  // SSD的内容已经在缓存中
};

class page_entry
{
public:
    page_entry(uint32_t blkoff);
    ~page_entry();

    void set_state(page_state sta)
    {
        std::lock_guard<std::mutex> lg(content_state_mtx);
        content_state = sta;
    }

    /* 对is_dirty进行CAS操作(由false更改为true)，成功返回true */
    bool mark_dirty()
    {
        bool expect = false;
        return is_dirty.compare_exchange_strong(expect, true);
    }

    /* to do */

private:
    uint32_t blkoff;   // 文件内块偏移
    uint32_t nid, nidoff;  // 反向映射

    page_state content_state;
    std::mutex content_state_mtx;  // 保护content_state的锁
    std::condition_variable cond;  // 等待其他线程读page的条件变量

    block_buffer page;
    rwlock_t page_rw_lock;  // page并发读写的锁

    /*
     * 引用计数，在调用方与page_entry_handle生命周期绑定，一个page_entry_handle增加1引用计数
     * page cache内的dirty list中的page entry也增加引用计数
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

    void mark_dirty();

private:
    page_cache *cache;
    page_entry *entry;

    void do_addref();
    void do_subref();
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

private:
    generic_cache_manager<uint32_t, page_entry> cache_manager;
    spinlock_t cache_lock;  // 保护cache_manager
    std::vector<page_entry*> dirty_list;
    spinlock_t dirty_list_lock;

    size_t expect_size, cur_size;

    /* 调用者需要加cache_lock，除非能够保证调用时ref_count不会为0 */
    void add_refcount(page_entry *entry);

    void sub_refcount(page_entry *entry);
    void do_replace();

    /* 
     * 由page_entry_handle的mark_dirty方法调用
     * 调用时能保证ref_count不为0，因为发起调用的page_entry_handle仍有效，所以内部不加cache_lock锁
     */
    void add_to_dirty_list(page_entry *entry);

    friend class page_entry_handle;
};

}  // namespace hscfs