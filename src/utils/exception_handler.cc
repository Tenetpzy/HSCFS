#include <unordered_map>
#include <typeindex>

#include "utils/exception_handler.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"
#include "fs/fs_manager.hh"

namespace hscfs {

using std::type_index;

std::unordered_map<std::type_index, int> exception_handler::recoverable_exceptions_errno = {
    {type_index(typeid(user_path_invalid)), EINVAL}
};

int exception_handler::convert_to_errno(bool set_unrecoverable)
{
    HSCFS_LOG(HSCFS_LOG_WARNING, "exception occurred: %s", e.what());
    int ret = ENOTRECOVERABLE;
    type_index type(typeid(e));
    if (recoverable_exceptions_errno.count(type) != 0)
        ret = recoverable_exceptions_errno.at(type);
    if (ret == ENOTRECOVERABLE && set_unrecoverable)
    {
        file_system_manager *fs_manager = file_system_manager::get_instance();
        fs_manager->set_unrecoverable();
    }

    return ret;
}

} // namespace hscfs