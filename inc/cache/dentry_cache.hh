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

class dentry
{
public:
    dentry_key key;
    uint32_t ino;   // 目录项的inode
    /* to do */
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
