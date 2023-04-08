#pragma once

#include "cache/block_buffer.hh"
#include "cache/cache_manager.hh"
#include "utils/hscfs_multithread.h"
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace hscfs {

enum class page_state
{
    uptodate,  // 缓存中的内容是最新的
    dirty,  // 缓存脏
    invalid,  // 缓存还没从SSD读上来
    reading  // 缓存正在由某个线程从SSD读
};

class page_entry
{
public:
    page_entry(uint32_t blkoff)
    {
        this->blkoff = blkoff;
        state = page_state::invalid;
    }

private:
    uint32_t blkoff;   // 文件内块偏移
    uint32_t nid, nidoff;  // 反向映射

    page_state state;
    std::mutex mtx;  // 保护state的锁
    std::condition_variable cond;  // 等待其他线程读page的条件变量

    block_buffer page;

    /* 当state为dirty时，保证贡献1点引用计数（page_entry_handle在脏链表中）*/
    std::atomic_uint32_t ref_count;
};

class page_cache;

class page_entry_handle
{
public:
    page_entry_handle(page_entry *entry, page_cache *cache)
    {
        this->entry = entry;
        this->cache = cache;
    }
    
    ~page_entry_handle();

    page_entry* operator->()
    {
        return entry;
    }

private:
    page_cache *cache;
    page_entry *entry;
};

/* 文件页缓存 */
class page_cache
{
public:
    page_cache(size_t expect_size);

    page_entry_handle get(uint32_t blkoff);

private:
    generic_cache_manager<uint32_t, page_entry> cache_manager;
    size_t expect_size, cur_size;
    spinlock_t lock;
};

}