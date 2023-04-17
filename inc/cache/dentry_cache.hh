#pragma once

#include <vector>
#include <string>
#include "cache/cache_manager.hh"

namespace hscfs {

struct dentry_key
{
    std::string name;  // 目录项名
    uint32_t dir_ino;  // 所属目录文件ino

    dentry_key() = default;
    dentry_key(uint32_t ino, const std::string &d_name)
        : name(d_name)
    {
        dir_ino = ino;
    }

    bool operator==(const dentry_key &o) const
    {
        return name == o.name && dir_ino == o.dir_ino;
    }
};

}  // namespace hscfs

namespace std {

template<>
struct hash<hscfs::dentry_key>
{
    size_t operator()(const hscfs::dentry_key &key) const
    {
        return hash<string>()(key.name) ^ hash<uint32_t>()(key.dir_ino);
    }
};

}  // namespace std

namespace hscfs {

class file_system_manager;

enum class dentry_state
{
    valid,
    deleted_inode_valid,
    deleted
};

class dentry
{
public:
    dentry(uint32_t dir_ino, dentry *parent, uint32_t dentry_ino, const std::string &dentry_name, 
        file_system_manager *fs_manager);

    uint32_t get_ino() const noexcept
    {
        return ino;
    }

    const dentry_key& get_key() const noexcept
    {
        return key;
    }

    /* 得到文件类型。若为UNKNOWN，则通过读inode page获取 */
    uint8_t get_type();

private:
    dentry_key key;
    uint32_t ino;  // 目录项的inode
    uint8_t type;  // 目录项的文件类型
    dentry *parent;  // 目录树中的父目录，根目录为nullptr

    uint32_t blkno, slotno;  // 目录项在目录文件中的位置(块号，块内slot号)
    bool is_dentry_pos_valid;  // blkno和slotno字段是否有效(由SSD path lookup返回的中间节点则不一定有效)
    
    file_system_manager *fs_manager;
    uint32_t ref_count;
    dentry_state state;

    /* 
     * 该目录项在目录文件中是否是新建/删除，但还没写回SSD的状态为dirty
     * 如果为dirty，保证至少有一个引用计数(cache的dirty list)
     */
    bool is_dirty;

    friend class dentry_cache;
};


class dentry_cache;

class dentry_handle
{
public:
    dentry_handle() noexcept
    {
        entry = nullptr;
        cache = nullptr;
    }

    dentry_handle(dentry *entry, dentry_cache *cache) noexcept
    {
        this->entry = entry;
        this->cache = cache;
    }

    dentry_handle(const dentry_handle &o)
    {
        entry = o.entry;
        cache = o.cache;
        do_addref();
    }

    dentry_handle(dentry_handle &&o) noexcept
    {
        entry = o.entry;
        cache = o.cache;
        o.entry = nullptr;
    }

    dentry_handle& operator=(const dentry_handle &o)
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

    dentry_handle& operator=(dentry_handle &&o)
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

    dentry* operator->() const noexcept
    {
        return entry;
    }

    void mark_dirty() const noexcept;

private:
    dentry *entry;
    dentry_cache *cache;

    void do_addref();
    void do_subref();

    friend class dentry_cache;
};

class dentry_cache
{
public:
    /* to do */

    dentry_handle add(uint32_t dir_ino, const dentry_handle &dir_handle, uint32_t dentry_ino, 
        const std::string &dentry_name, file_system_manager *fs_manager)
    {
        assert(cache_manager.get(dentry_key(dir_ino, dentry_name)) == nullptr);

        /* 构造新的dentry */
        dentry *parent = dir_handle.entry;
        assert(parent != nullptr);
        assert(cache_manager.get(parent->key, false) != nullptr);
        auto p_entry = std::make_unique<dentry>(dir_ino, parent, dentry_ino, dentry_name, fs_manager);

        /* 将parent的引用计数+1 */
        add_refcount(parent);

        /* 将新dentry加入缓存，若数量超过阈值，进行置换 */
        dentry *raw_p = p_entry.get();
        cache_manager.add(p_entry->key, p_entry);
        add_refcount(raw_p);
        ++cur_size;
        do_replace();

        return dentry_handle(raw_p, this);
    }

    dentry_handle get(uint32_t dir_ino, const std::string &name)
    {
        dentry *entry = cache_manager.get(dentry_key(dir_ino, name));
        if (entry != nullptr)
            add_refcount(entry);
        return dentry_handle(entry, this);
    }

private:
    size_t expect_size, cur_size;
    generic_cache_manager<dentry_key, dentry> cache_manager;
    std::vector<dentry_handle> dirty_list;

    void add_refcount(dentry *entry)
    {
        ++entry->ref_count;
        /* 引用计数同时维护了淘汰保护、脏、被引用、读写等状态。只要引用计数不为0，就需要pin */
        if (entry->ref_count == 1)
            cache_manager.pin(entry->key);
    }

    void sub_refcount(dentry *entry)
    {
        --entry->ref_count;
        if (entry->ref_count == 0)
            cache_manager.unpin(entry->key);
    }

    void do_replace();

    void mark_dirty(const dentry_handle &handle)
    {
        if (!handle.entry->is_dirty)
        {
            handle.entry->is_dirty = true;
            dirty_list.emplace_back(handle);
        }
    }

    friend class dentry_handle;
};

}  // namespace hscfs
