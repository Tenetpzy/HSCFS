#pragma once

#include "cache/block_buffer.hh"
#include "cache/cache_manager.hh"

namespace hscfs {

enum class dir_data_block_cache_entry_state
{
    uptodate, dirty
};

struct dir_data_block_cache_entry_key
{
    uint32_t ino;  // 所属目录文件inode号
    uint64_t blkno;  // 该block的目录文件块偏移

    bool operator==(const dir_data_block_cache_entry_key &rhs) const
    {
        return ino == rhs.ino && blkno == rhs.blkno;
    }
};

class dir_data_block_cache_entry
{
public:
    /* to do */

private:
    dir_data_block_cache_entry_key key;  // hash key
    uint32_t origin_lpa, commit_lpa;  // block的旧lpa，和写入时应写的新lpa
    uint32_t nid, nidoff;  // block的反向索引映射
    dir_data_block_cache_entry_state state;
    /* to do: related dentry list */
    block_buffer block;
};

}  // namespace hscfs

/* 提供对hscfs::dir_data_block_cache_entry_key的哈希函数特化 */
namespace std {
    template<> 
    struct hash<hscfs::dir_data_block_cache_entry_key>
    {
        static const uint64_t mul = 0x1f1f1f1f1f1f1f1f;
        size_t operator()(const hscfs::dir_data_block_cache_entry_key &key) const
        {
            return (static_cast<uint64_t>(key.ino) * mul) ^ key.blkno;
        }
    };
}

namespace hscfs {

class dir_data_block_cache
{
    /* to do */
};

}