#pragma once

#include <cstdint>
#include "communication/comm_api.h"
#include "fs/SIT_utils.hh"
#include "fs/replace_proc.hh"

namespace hscfs {

class super_manager;
class file_system_manager;
class block_buffer;

/* 写回时使用的工具类。使用此类应先获取fs_meta_lock */
class write_back_helper
{
public:
    enum class block_type {
        node, data
    };

public:
    write_back_helper(file_system_manager *fs_manager);

    /* 
     * 将buffer对应的block回写。lpa引用缓存项的lpa字段
     * 此方法内部：
     * 1. 为block分配一个新的物理LPA，将新LPA有效化
     * 2. 根据lpa的状态，将block的旧LPA无效化(如果有旧LPA)
     * 3. 将lpa更新为新LPA
     * 4. 将buffer异步写入新LPA
     * 5. 返回新LPA
     * 注意：不更新任何反向映射
     */
    uint32_t do_write_back_async(block_buffer &buffer, uint32_t &lpa, block_type type, 
        comm_async_cb_func cb_func, void *cb_arg);

    /*
     * 生成一个元数据回写事务，将文件系统中所有脏元数据回写
     * 返回该回写事务的淘汰保护信息
     */
    transaction_replace_protect_record write_meta_back_sync();

private:
    file_system_manager *fs_manager;
    super_manager *super;
    SIT_operator sit_operator;
};

}  // namespace hscfs