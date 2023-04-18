#pragma once

#include "cache/block_buffer.hh"
#include "cache/cache_manager.hh"
#include <cassert>
#include <vector>

namespace hscfs {

enum class dir_data_block_entry_state
{
    uptodate, dirty
};

struct dir_data_block_entry_key
{
    uint32_t ino;  // 所属目录文件inode号
    uint32_t blkoff;  // 该block的目录文件块偏移

    dir_data_block_entry_key(uint32_t ino, uint32_t blkoff)
    {
        this->ino = ino;
        this->blkoff = blkoff;
    }

    bool operator==(const dir_data_block_entry_key &rhs) const
    {
        return ino == rhs.ino && blkoff == rhs.blkoff;
    }
};

class dir_data_block_entry
{
public:
    dir_data_block_entry(uint32_t ino, uint32_t blkoff, uint32_t lpa, uint32_t nid, 
        uint32_t nidoff, block_buffer &&block) noexcept
        : key(ino, blkoff), block(std::move(block))
    {
        origin_lpa = lpa;
        this->nid = nid;
        this->nidoff = nidoff;
        state = dir_data_block_entry_state::uptodate;
        ref_count = 0;
    }
    ~dir_data_block_entry();

    const dir_data_block_entry_key& get_key() const noexcept
    {
        return key;
    }

private:
    dir_data_block_entry_key key;  // hash key
    uint32_t origin_lpa, commit_lpa;  // block的旧lpa，和写入时应写的新lpa
    uint32_t nid, nidoff;  // block的反向索引映射
    block_buffer block;

    dir_data_block_entry_state state;
    uint32_t ref_count;

    friend class dir_data_block_cache;
};

class dir_data_block_cache;

class dir_data_block_handle
{
public:
    dir_data_block_handle(dir_data_block_entry *entry, dir_data_block_cache *cache) noexcept
    {
        this->entry = entry;
        this->cache = cache;
    }

    dir_data_block_handle(const dir_data_block_handle &o)
    {
        entry = o.entry;
        cache = o.cache;
        do_addref();
    }

    dir_data_block_handle(dir_data_block_handle &&o) noexcept
    {
        entry = o.entry;
        cache = o.cache;
        o.entry = nullptr;
    }

    dir_data_block_handle& operator=(const dir_data_block_handle &o)
    {
        if (this != &o)
        {
            do_subref();
            cache = o.cache;
            entry = o.entry;
            do_addref();
        }
        return *this;
    }

    dir_data_block_handle& operator=(dir_data_block_handle &&o)
    {
        if (this != &o)
        {
            do_subref();
            cache = o.cache;
            entry = o.entry;
            o.entry = nullptr;
        }
        return *this;
    }

    ~dir_data_block_handle();

    dir_data_block_entry* operator->()
    {
        return entry;
    }

    void mark_dirty();

    bool is_empty() const noexcept
    {
        return entry == nullptr;
    }

private:
    dir_data_block_entry *entry;
    dir_data_block_cache *cache;

    void do_addref();
    void do_subref();

    friend class dir_data_block_cache;
};

}  // namespace hscfs

/* 提供对hscfs::dir_data_block_entry_key的哈希函数特化 */
namespace std {
    template<> 
    struct hash<hscfs::dir_data_block_entry_key>
    {
        static const uint32_t mul = 0x1f1f1f1f;
        size_t operator()(const hscfs::dir_data_block_entry_key &key) const
        {
            return (key.ino * mul) ^ key.blkoff;
        }
    };
}

namespace hscfs {

class dir_data_block_cache
{
public:
    dir_data_block_cache(size_t expect_size) 
    {
        this->expect_size = expect_size;
        cur_size = 0;
    }

    ~dir_data_block_cache();

    /*
     * 将一个dir data block加入缓存，返回缓存项handle
     * 加入缓存时，该缓存项引用计数为0，且data block语义上应为uptodate状态
     * 调用add后，block参数资源被移动，不再有效，调用者应使用返回的handle
     */
    dir_data_block_handle add(uint32_t ino, uint32_t blkoff, uint32_t lpa, uint32_t nid, 
        uint32_t nidoff, block_buffer &&block)
    {
        auto p_entry = std::make_unique<dir_data_block_entry>(ino, blkoff, lpa, nid, nidoff, std::move(block));
        const dir_data_block_entry_key &key = p_entry->get_key();
        assert(cache_manager.get(key) == nullptr);

        dir_data_block_entry *raw_p = p_entry.get();
        cache_manager.add(key, p_entry);
        ++cur_size;
        add_refcount(raw_p);
        do_relpace();

        return dir_data_block_handle(raw_p, this);
    }

    dir_data_block_handle get(uint32_t ino, uint32_t blkoff)
    {
        dir_data_block_entry_key key(ino, blkoff);
        auto p_entry = cache_manager.get(key);
        if (p_entry != nullptr)
            add_refcount(p_entry);
        return dir_data_block_handle(p_entry, this);
    }

private:
    size_t expect_size, cur_size;
    generic_cache_manager<dir_data_block_entry_key, dir_data_block_entry> cache_manager;
    std::vector<dir_data_block_handle> dirty_list;

private:
    void add_refcount(dir_data_block_entry *entry)
    {
        ++entry->ref_count;
        /* dirty状态会增加一点引用计数，并加入dirty_list中，所以不需要额外判断dirty */
        if (entry->ref_count == 1)
            cache_manager.pin(entry->get_key());
    }

    void sub_refcount(dir_data_block_entry *entry)
    {
        --entry->ref_count;
        if (entry->ref_count == 0)
            cache_manager.unpin(entry->get_key());
    }

    void mark_dirty(const dir_data_block_handle &handle)
    {
        if (handle.entry->state != dir_data_block_entry_state::dirty)
        {
            handle.entry->state = dir_data_block_entry_state::dirty;
            dirty_list.emplace_back(handle);
        }
    }

    void do_relpace();

    friend class dir_data_block_handle;
};

}