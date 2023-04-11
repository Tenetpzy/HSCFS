#pragma once

#include <string>

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

class super_manager;

enum class dentry_state
{
    valid,
    deleted_inode_valid,
    deleted
};

class dentry
{
public:
    dentry_key key;
    uint32_t ino;  // 目录项的inode
    super_manager *super;
    dentry *parent;  // 目录树中的父目录，根目录为nullptr

    uint32_t blkno, slotno;  // 目录项在目录文件中的位置(块号，块内slot号)
    bool is_dentry_pos_valid;  // blkno和slotno字段是否有效(由SSD path lookup返回的中间节点则不一定有效)

    uint32_t ref_count;
    dentry_state state;
};

class dentry_handle
{
    /* to do */
};

class dentry_cache
{
    /* to do */
};

}  // namespace hscfs
