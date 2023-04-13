#pragma once

#include <memory>
#include <ctime>
#include "cache/dentry_cache.hh"
#include <utils/hscfs_multithread.h>

namespace hscfs {

class page_cache;
class file_system_manager;

/* 
 * 作用类似VFS的inode，代表文件系统中的一个文件
 *
 * 但是file只代表非目录文件，目录由directory对象表示
 * 做区分的原因：文件系统层使用global_lock，但对文件的读写需要并发，
 * 为了防止文件读写时也需要获取global_lock，将file对象的并发单独管理
 */
class file
{
public:

protected:
    const uint32_t ino;  // inode号

    uint64_t size;  // 当前文件大小（字节数）
    timespec atime;  // 访问时间戳
    timespec mtime;  // 修改时间戳（ctime == mtime，不设ctime字段）
    bool is_dirty;
    spinlock_t file_meta_lock;  // 保护size、atime、mtime、is_dirty的锁

    std::unique_ptr<page_cache> page_cache_;
    rwlock_t page_cache_lock;
    
    file_system_manager *const fs_manager;

    uint32_t nlink;  // 硬连接数，访问时加fs_lock
    uint32_t ref_count;  // 引用计数，访问时加fs_lock
};

/* file对象的缓存 */
class file_obj_cache
{
    /* to do */
};

} // namespace hscfs
