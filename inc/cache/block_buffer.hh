#pragma once

#include "utils/declare_utils.hh"

namespace hscfs {

/* 块缓存区的容器，构造时分配4KB块缓存区，析构时释放 */
class block_buffer
{
public:
    block_buffer();
    block_buffer(const block_buffer&);
    block_buffer(block_buffer&&) noexcept;
    block_buffer &operator=(const block_buffer&);
    block_buffer &operator=(block_buffer&&) noexcept;
    ~block_buffer();

    char *get_ptr() noexcept
    {
        return buffer;
    }

private:
    char *buffer;
};

}  // namespace hscfs