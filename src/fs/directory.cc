#include "fs/directory.hh"
#include "fs/fs_manager.hh"
#include "fs/fs.h"

namespace hscfs {

dentry_info::dentry_info()
{
    ino = INVALID_NID;
}

dentry_handle directory::create(const std::string &name, uint8_t type, const dentry_store_pos *create_pos_hint)
{
    /* 注意，可能有同名的老dentry，但它处于deleted状态，此时不应该出错 */
    
}

dentry_info directory::lookup(const std::string &name)
{
    
}

} // namespace hscfs