#pragma once

#include <string>
#include "cache/cache_manager.hh"

namespace hscfs {

struct dentry_key
{
    std::string name;  // 目录项名
    uint32_t dir_ino;  // 所属目录文件ino

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
    
    uint32_t get_ino() const noexcept
    {
        return ino;
    }

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
    bool is_sync_with_SSD;
};


class dentry_cache;

class dentry_handle
{
public:
    dentry_handle(dentry *entry, dentry_cache *cache)
    {
        this->entry = entry;
        this->cache = cache;
    }

    bool is_empty() const noexcept
    {
        return entry == nullptr;
    }

    dentry* operator->() const noexcept
    {
        return entry;
    }

    /* to do */

private:
    dentry *entry;
    dentry_cache *cache;
};

class dentry_cache
{
public:
    /* to do */

    dentry_handle get(uint32_t dir_ino, const std::string &name);

private:
    size_t expect_size, cur_size;
    generic_cache_manager<dentry_key, dentry> cache_manager;
};

}  // namespace hscfs
