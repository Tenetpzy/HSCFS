#pragma once

#include <memory>

namespace hscfs {

/*
 * super_manager, SIT cache, NAT cache等对象的组合容器
 */
class file_system_manager
{
public:
    file_system_manager* get_instance();

private:

};

}  // namespace hscfs