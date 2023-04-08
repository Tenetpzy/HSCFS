#include "cache/page_cache.hh"
#include "utils/spin_lock_guard.hh"
#include <system_error>

namespace hscfs {

page_cache::page_cache(size_t expect_size)
{
    int ret = spin_init(&lock);
    if (ret != 0)
        throw std::system_error(std::error_code(ret, std::generic_category()), "page cache: init spin failed.");
    this->expect_size = expect_size;
    cur_size = 0;
}



}