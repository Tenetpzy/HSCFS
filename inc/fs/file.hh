#pragma once

#include <memory>
#include <ctime>
#include "cache/dentry_cache.hh"

namespace hscfs {

class page_cache;
class file_system_manager;

// 作用类似VFS的inode，代表文件系统中的一个文件
class file
{
public:
    


protected:
    uint32_t ino;  // inode号
    uint64_t size;  // 当前文件大小（字节数）
    timespec atime;  // 访问时间戳
    timespec mtime;  // 修改时间戳（ctime == mtime，不设ctime字段）
    uint32_t nlink;  // 硬连接数
    file_system_manager *fs_manager;
};

// 封装file引用计数管理
class file_handle
{
    /* to do */
};

/* file对象的缓存 */
class file_obj_cache
{
    /* to do */
};


class generic_file: public file
{

};

} // namespace hscfs
