#pragma once

#include <vector>
#include <string>
#include <cassert>
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
    deleted
};

struct dentry_store_pos
{
    uint32_t blkno, slotno;  // dentry在目录文件中的块号和块内slot号
    bool is_valid;  // 信息是否有效

    dentry_store_pos();

    bool operator==(const dentry_store_pos &o) const noexcept
    {
        return is_valid == o.is_valid && blkno == o.blkno && slotno == o.slotno;
    }

    /* 设置位置信息，置is_valid为true */
    void set_pos(uint32_t blkno, uint32_t slotno)
    {
        this->blkno = blkno;
        this->slotno = slotno;
        is_valid = true;
    }
};

class dentry
{
public:
    /* 构造后，状态置为valid，is_dirty置为false, pos无效, ref_count均为0 */
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

    const dentry_key& get_parent_key() const noexcept
    {
        if (parent == nullptr)  // 如果是根目录
            return key;
        return parent->get_key();
    }

    /* 得到文件类型。若为UNKNOWN，则通过读inode page获取 */
    uint8_t get_type();

    dentry_state get_state() const noexcept
    {
        return state;
    }

    void set_state(dentry_state sta) noexcept
    {
        state = sta;
    }

    void set_pos_info(const dentry_store_pos &p) noexcept
    {
        pos = p;
    }

    const dentry_store_pos& get_pos_info() const noexcept
    {
        return pos;
    }

    /* 当dentry为deleted，且又需要创建一个同名目录项时，可直接使用以下两个方法设置新属性 */
    void set_ino(uint32_t ino) noexcept
    {
        this->ino = ino;
    }

    void set_type(uint8_t type) noexcept
    {
        this->type = type;
    }

private:
    dentry_key key;
    uint32_t ino;  // 目录项的inode
    uint8_t type;  // 目录项的文件类型
    dentry *parent;  // 目录树中的父目录，根目录则为nullptr

    dentry_store_pos pos;
    
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

    dentry_handle& operator=(dentry_handle &&o) noexcept
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

    /* 封装目录项是否存在的检查 */
    bool is_exist() const noexcept
    {
        return entry != nullptr && entry->get_state() == dentry_state::valid;
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
    dentry_cache(size_t expect_size, file_system_manager *fs_manager)
    {
        this->expect_size = expect_size;
        this->fs_manager = fs_manager;
        cur_size = 0;
    }

    ~dentry_cache();

    /* 新增一个目录项，目录项状态为dentry构造后的默认状态 */
    dentry_handle add(uint32_t dir_ino, const dentry_handle &dir_handle, uint32_t dentry_ino, 
        const std::string &dentry_name)
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

    dentry_handle add_root(uint32_t root_ino)
    {
        auto p_root = std::make_unique<dentry>(root_ino, nullptr, root_ino, "/", fs_manager);
        dentry *raw_p = p_root.get();
        cache_manager.add(p_root->key, p_root);
        add_refcount(raw_p);
        ++cur_size;
        return dentry_handle(raw_p, this);
    }

    dentry_handle get(uint32_t dir_ino, const std::string &name)
    {
        dentry *entry = cache_manager.get(dentry_key(dir_ino, name));
        if (entry != nullptr)
            add_refcount(entry);
        return dentry_handle(entry, this);
    }

    std::vector<dentry_handle> get_and_clear_dirty_list()
    {
        std::vector<dentry_handle> ret;
        dirty_list.swap(ret);
        assert(dirty_list.size() == 0);
        return ret;
    }

private:
    size_t expect_size, cur_size;
    file_system_manager *fs_manager;
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
