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
 */
class file
{
public:

    /* to do. inode操作的接口。generic_file和directory分别提供实现 */

protected:
    uint32_t ino;  // inode号

    uint64_t size;  // 当前文件大小（字节数）
    timespec atime;  // 访问时间戳
    timespec mtime;  // 修改时间戳（ctime == mtime，不设ctime字段）
    bool is_dirty;
    
    file_system_manager *fs_manager;

    uint32_t nlink;  // 硬连接数，访问时加fs_lock
    uint32_t ref_count;  // 引用计数，访问时加fs_lock
};

class generic_file: public file
{
public:
    /* to do */
private:
    /* 保护size、atime、mtime、is_dirty的锁，对于文件的file对象，会被page cache层并发访问 */
    spinlock_t file_meta_lock;

    /* 
     * 对文件操作的锁。对generic_file进行任何操作前，必须获取该锁的共享/独占
     * 此锁的层级在file_mata_lock上。只在需要修改file中元数据时获取file_meta_lock
     */
    rwlock_t file_op_lock;

    std::unique_ptr<page_cache> page_cache_;
};

class directory: public file
{
public:
    /* to do */
private:
};

/* file对象的缓存 */
class file_obj_cache
{
    /* to do */
};

} // namespace hscfs
