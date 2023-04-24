#pragma once

#include <cstdint>

namespace hscfs {

class file_system_manager;
class super_cache;

/* 负责超级块中的资源分配、释放和维护 */
class super_manager
{
public:
    super_manager(file_system_manager *fs_manager);

    /* 释放nid，把它加入空闲nid链表，并将修改记录到super和NAT日志 */
    void free_nid(uint32_t nid);

private:
    file_system_manager *fs_manager;
    super_cache *super;
};

}