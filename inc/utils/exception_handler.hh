#pragma once

#include <stdexcept>
#include <unordered_map>
#include <typeindex>

namespace hscfs {

/* 异常处理器 */
class exception_handler
{
public:
    exception_handler(const std::exception &except): e(except) {}

    /* 
     * 转换异常对象到errno
     *
     * 若set_unrecoverable为true，则异常对象为不可恢复异常时，将fs_manager的状态设置为不可恢复
     * fs_manager置为不可恢复状态后，将不再向SSD提交事务，且对于大多数API，将返回ENOTRECOVERABLE
     * 此时调用者应持有fs_meta_lock锁 
     */
    int convert_to_errno(bool set_unrecoverable = false);
    
private:
    const std::exception &e;

    static std::unordered_map<std::type_index, int> recoverable_exceptions_errno;
};

}  // namespace hscfs