#include <unordered_map>
#include <typeindex>

#include "utils/exception_handler.hh"
#include "utils/hscfs_exceptions.hh"
#include "utils/hscfs_log.h"

namespace hscfs {

int handle_exception(const std::exception &e)
{
    using std::type_index;
    static const std::unordered_map<type_index, int> exception_to_errno = {
        {type_index(typeid(user_path_invalid)), EINVAL}
    };

    HSCFS_LOG(HSCFS_LOG_WARNING, "exception occurred: %s", e.what());
    int ret = ENOTRECOVERABLE;
    type_index type(typeid(e));
    if (exception_to_errno.count(type) != 0)
        ret = exception_to_errno.at(type);
    return ret;
}

} // namespace hscfs