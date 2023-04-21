#include "fs/directory.hh"
#include "fs/fs_manager.hh"
#include "directory.hh"

namespace hscfs {

dentry_handle directory::create(const std::string &name, uint8_t type, const dentry_store_pos *create_pos_hint)
{
    /* 注意，可能有同名的老dentry，但它处于deleted状态，此时不应该出错 */
}

dentry_handle directory::lookup(const std::string &name)
{
    dentry_cache *d_cache = fs_manager->get_dentry_cache();
    dentry_handle target = d_cache->get(ino, name);

    /* 如果目录项缓存不命中，则在目录文件中查找 */
    if (target.is_empty())
    {
        dentry_info d_info = lookup_in_dirfile(name);
    }
}

dentry_info directory::lookup_in_dirfile(const std::string &name)
{
    
}

} // namespace hscfs