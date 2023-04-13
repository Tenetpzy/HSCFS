#pragma once

#include <memory>
#include <mutex>

namespace hscfs {

class super_cache;

/*
 * super_manager, SIT cache, NAT cache等对象的组合容器
 */
class file_system_manager
{
public:
    file_system_manager* get_instance();

    std::mutex& get_fs_lock_ref() noexcept
    {
        return fs_lock;
    }

    super_cache* get_super_cache() const noexcept
    {
        return super.get();
    }

private:

    std::mutex fs_lock;
    std::unique_ptr<super_cache> super;
};

}  // namespace hscfs