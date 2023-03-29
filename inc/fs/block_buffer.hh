#pragma once

#include "utils/declare_utils.hh"

namespace hscfs {

class block_buffer
{
public:
    block_buffer();
    block_buffer(const block_buffer&);
    block_buffer(block_buffer&&) noexcept;
    block_buffer &operator=(const block_buffer&);
    block_buffer &operator=(block_buffer&&) noexcept;
    ~block_buffer();

    char *get_ptr() const noexcept
    {
        return buffer;
    }

private:
    char *buffer;
};

}  // namespace hscfs