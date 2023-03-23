#pragma once

#include "utils/declare_utils.hh"

class hscfs_block_buffer
{
public:
    hscfs_block_buffer();
    hscfs_block_buffer(const hscfs_block_buffer&);
    hscfs_block_buffer(hscfs_block_buffer&&) noexcept;
    hscfs_block_buffer &operator=(const hscfs_block_buffer&);
    hscfs_block_buffer &operator=(hscfs_block_buffer&&) noexcept;
    ~hscfs_block_buffer();

    char *get_ptr() const noexcept
    {
        return buffer;
    }

private:
    char *buffer;
};