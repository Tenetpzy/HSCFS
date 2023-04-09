#pragma once

#include "cache/cache_manager.hh"
#include "cache/block_buffer.hh"
#include "fs/fs.h"

#include <cassert>

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
        this->is_pinned = false;
    }

    ~node_block_cache_entry();

    uint32_t nid;
    uint32_t parent_nid;
    uint32_t old_lpa, new_lpa;
    block_buffer node;

    uint32_t ref_count;
    node_block_cache_entry_state state;
    bool is_pinned;

    bool need_pinned() const noexcept
    {
        if (is_pinned)
            return false;
        return ref_count > 0 || state == node_block_cache_entry_state::dirty;
    }

    bool can_unpin() const noexcept
    {
        return ref_count == 0 && state == node_block_cache_entry_state::uptodate;
    }
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

    node_block_cache_entry_handle(const node_block_cache_entry_handle &o);

    node_block_cache_entry_handle(node_block_cache_entry_handle &&o) noexcept
    {
        entry = o.entry;
        cache = o.cache;
        o.entry = nullptr;
    }

    node_block_cache_entry_handle& operator=(const node_block_cache_entry_handle &o);
    node_block_cache_entry_handle& operator=(node_block_cache_entry_handle &&o);

    ~node_block_cache_entry_handle();

    bool is_empty() const noexcept
    {
        return entry == nullptr;
    }

    void add_host_version();
    void add_SSD_version();
    void mark_dirty();
    void clear_dirty();
    
    uint32_t get_old_lpa() const noexcept {
        return entry->old_lpa;
    }
    uint32_t get_new_lpa() const noexcept {
        return entry->new_lpa;
    }
    void set_new_lpa(uint32_t new_lpa) noexcept {
        entry->new_lpa = new_lpa;
    }
    hscfs_node *get_node_block_ptr() const noexcept {
        return reinterpret_cast<hscfs_node*>(entry->node.get_ptr());
    }

private:
    node_block_cache_entry *entry;
    node_block_cache *cache;

    void do_addref();
    void do_subref();
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

    /*
     * 将一个node block加入缓存，返回缓存项句柄
     * 加入时，调用者需保证加入的buffer满足：ref_count为0(不存在读写引用，双版本号相同)，语义上为uptodate状态
     * 通常，只应当在缓存未命中时，从SSD读取或执行file mapping任务，并add结果
     * 
     * 调用add后，buffer的资源被移动到缓存项中，调用者若要使用buffer，应通过返回的handle获取buffer
     * parent_nid为索引树上的父node block，若此node为inode，则parent_nid应置为INVALID_NID
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

private:
    size_t expect_size, cur_size;
    generic_cache_manager<uint32_t, node_block_cache_entry> cache_manager;

private:
    void pin(node_block_cache_entry *entry)
    {
        assert(entry->is_pinned == false);
        entry->is_pinned = true;
        cache_manager.pin(entry->nid);
    }

    void unpin(node_block_cache_entry *entry)
    {
        assert(entry->is_pinned == true);
        entry->is_pinned = false;
        cache_manager.unpin(entry->nid);
        do_replace();  // 若缓存项数量已超出阈值，被unpin的缓存项将立刻换出
    }

    void add_refcount(node_block_cache_entry *entry)
    {
        ++entry->ref_count;
        if (entry->need_pinned())
            pin(entry);
    }

    void sub_refcount(node_block_cache_entry *entry)
    {
        --entry->ref_count;
        if (entry->can_unpin())
            unpin(entry);
    }

    void mark_dirty(node_block_cache_entry *entry)
    {
        entry->state = node_block_cache_entry_state::dirty;
        if (entry->need_pinned())
            pin(entry);
    }

    void clear_dirty(node_block_cache_entry *entry)
    {
        entry->state = node_block_cache_entry_state::uptodate;
        if (entry->can_unpin())
            unpin(entry);
    }

    void do_replace();

    friend class node_block_cache_entry_handle;
};

}  // namespace hscfs