#pragma once

#include "cache/cache_manager.hh"
#include "cache/block_buffer.hh"
#include "fs/fs.h"

#include <cassert>
#include <vector>

struct comm_dev;

namespace hscfs {

enum class node_block_cache_entry_state
{
    uptodate, dirty
};

class node_block_cache_entry
{
public:
    node_block_cache_entry(block_buffer &&buffer, uint32_t nid, uint32_t parent_nid, uint32_t old_lpa)
        : node(std::move(buffer))
    {
        this->nid = nid;
        this->parent_nid = parent_nid;
        this->old_lpa = old_lpa;
        this->new_lpa = INVALID_LPA;
        this->ref_count = 0;
        this->state = node_block_cache_entry_state::uptodate;
    }

    ~node_block_cache_entry();

    uint32_t get_old_lpa() const noexcept {
        return old_lpa;
    }

    uint32_t get_new_lpa() const noexcept {
        return new_lpa;
    }

    void set_new_lpa(uint32_t new_lpa) noexcept {
        this->new_lpa = new_lpa;
    }

    hscfs_node *get_node_block_ptr() noexcept {
        return reinterpret_cast<hscfs_node*>(node.get_ptr());
    }

private:
    uint32_t nid;
    uint32_t parent_nid;
    uint32_t old_lpa, new_lpa;
    block_buffer node;

    uint32_t ref_count;
    node_block_cache_entry_state state;

    friend class node_block_cache;
};

class node_block_cache;

/* 句柄的作用同sit/nat缓存 */
class node_block_cache_entry_handle
{
public:
    node_block_cache_entry_handle() noexcept
    {
        entry = nullptr;
        cache = nullptr;
    }

    node_block_cache_entry_handle(node_block_cache_entry *entry, node_block_cache *cache) noexcept
    {
        this->entry = entry;
        this->cache = cache;
    }

    node_block_cache_entry_handle(const node_block_cache_entry_handle &o)
    {
        entry = o.entry;
        cache = o.cache;
        do_addref();
    }

    node_block_cache_entry_handle(node_block_cache_entry_handle &&o) noexcept
    {
        entry = o.entry;
        cache = o.cache;
        o.entry = nullptr;
    }

    node_block_cache_entry_handle& operator=(const node_block_cache_entry_handle &o)
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

    node_block_cache_entry_handle& operator=(node_block_cache_entry_handle &&o)
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

    ~node_block_cache_entry_handle();

    bool is_empty() const noexcept
    {
        return entry == nullptr;
    }

    void add_host_version();
    void add_SSD_version();
    void mark_dirty() const noexcept;

    node_block_cache_entry* operator->()
    {
        return entry;
    }

private:
    node_block_cache_entry *entry;
    node_block_cache *cache;

    void do_addref();
    void do_subref();
    
    friend class node_block_cache;
};

class node_block_cache
{
public:
    /* expect_cache_size参数含义同SIT_NAT_cache */
    node_block_cache(size_t expect_cache_size)
    {
        expect_size = expect_cache_size;
        cur_size = 0;
    }

    ~node_block_cache();

    /*
     * 将一个node block加入缓存，返回缓存项句柄
     * 加入时，调用者需保证加入的buffer满足：ref_count为0(不存在读写引用，双版本号相同)，语义上为uptodate状态
     * 通常，只应当在缓存未命中时，从SSD读取或执行file mapping任务，并add结果
     * 
     * 调用add后，buffer的资源被移动到缓存项中，调用者若要使用buffer，应通过返回的handle获取buffer
     * parent_nid为索引树上的父node block，若此node为inode，则parent_nid应置为INVALID_NID
     * 调用者应确保parent_nid在缓存中
     */
    node_block_cache_entry_handle add(block_buffer &&buffer, uint32_t nid, uint32_t parent_nid, uint32_t old_lpa)
    {
        assert(cache_manager.get(nid, false) == nullptr);

        // 构造新的缓存项
        auto p_entry = std::make_unique<node_block_cache_entry>(std::move(buffer), nid, parent_nid, old_lpa);

        // 将parent_nid的引用计数+1
        if (parent_nid != INVALID_NID)
        {
            node_block_cache_entry *p_parent = cache_manager.get(parent_nid, false);
            assert(p_parent != nullptr);
            add_refcount(p_parent);
        }

        // 将缓存项加入cache_manager，并尝试进行置换(若加入后缓存数量超过阈值)
        node_block_cache_entry *raw_p = p_entry.get();
        cache_manager.add(nid, p_entry);
        add_refcount(raw_p);
        ++cur_size;
        do_replace();

        return node_block_cache_entry_handle(raw_p, this);
    }
    
    /*
     * 查找nid对应的缓存项
     * 若不存在，则句柄的is_empty方法返回true
     * 视作对缓存项的一次访问
     */
    node_block_cache_entry_handle get(uint32_t nid)
    {
        node_block_cache_entry *p_entry = cache_manager.get(nid);
        if (p_entry != nullptr)
            add_refcount(p_entry);
        return node_block_cache_entry_handle(p_entry, this);
    }

    std::vector<node_block_cache_entry_handle> get_dirty_list() const noexcept
    {
        return dirty_list;
    }

    /* 将dirty list中的缓存项置为uptodate状态，并清空dirty list */
    void clear_dirty_list()
    {
        for (auto &handle :dirty_list)
        {
            assert(handle.entry->state == node_block_cache_entry_state::dirty &&
                handle.entry->ref_count >= 1);
            handle.entry->state = node_block_cache_entry_state::uptodate;
        }
        dirty_list.clear();
    }

    void force_replace()
    {
        do_replace();
    }

private:
    size_t expect_size, cur_size;
    generic_cache_manager<uint32_t, node_block_cache_entry> cache_manager;
    std::vector<node_block_cache_entry_handle> dirty_list;

private:
    void add_refcount(node_block_cache_entry *entry)
    {
        ++entry->ref_count;
        /* 引用计数同时维护了淘汰保护、脏、被引用、读写等状态。只要引用计数不为0，就需要pin */
        if (entry->ref_count == 1)
            cache_manager.pin(entry->nid);
    }

    void sub_refcount(node_block_cache_entry *entry)
    {
        --entry->ref_count;
        if (entry->ref_count == 0)
            cache_manager.unpin(entry->nid);
    }

    void mark_dirty(const node_block_cache_entry_handle &handle)
    {
        /* 保证node block缓存项如果是dirty状态，一定至少有一个引用计数 */
        if (handle.entry->state != node_block_cache_entry_state::dirty)
        {
            handle.entry->state = node_block_cache_entry_state::dirty;
            dirty_list.emplace_back(handle);
        }
    }

    void do_replace();

    friend class node_block_cache_entry_handle;
};

class SIT_NAT_cache;
class file_system_manager;

/*
 * node block获取器
 * 封装node block cache不命中时，从NAT表中查找lpa并读入缓存的过程
 */
class node_cache_helper
{
public:
    node_cache_helper(file_system_manager *fs_manager) noexcept;

    /* 
     * 调用时持有fs_meta_lock 
     * parent_nid为目标nid在索引树上的父node block。如果nid为inode block，则parent_nid应置为INVALID_NID
     * 调用者应确保parent_nid在缓存中
     */
    node_block_cache_entry_handle get_node_entry(uint32_t nid, uint32_t parent_nid);

private:

    comm_dev *dev;
    SIT_NAT_cache *nat_cache;
    node_block_cache *node_cache;
    file_system_manager *fs_manager;
};

}  // namespace hscfs