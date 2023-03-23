#include "fs/block_buffer.hh"
#include "communication/memory.h"
#include "utils/hscfs_exceptions.hh"
#include <cstring>

hscfs_block_buffer::hscfs_block_buffer()
{
    buffer = static_cast<char*>(comm_alloc_dma_mem(4096));
    if (buffer == nullptr)
        throw hscfs_alloc_error("alloc block buffer failed.");
}

hscfs_block_buffer::hscfs_block_buffer(const hscfs_block_buffer &o): hscfs_block_buffer()
{
    std::memcpy(buffer, o.buffer, 4096);
}

hscfs_block_buffer::hscfs_block_buffer(hscfs_block_buffer &&o) noexcept
{
    if (this != &o)
    {
        buffer = o.buffer;
        o.buffer = nullptr;
    }
}

hscfs_block_buffer &hscfs_block_buffer::operator=(const hscfs_block_buffer &o)
{
    std::memcpy(buffer, o.buffer, 4096);
    return *this;
}

hscfs_block_buffer &hscfs_block_buffer::operator=(hscfs_block_buffer &&o) noexcept
{
    if (this != &o)
    {
        comm_free_dma_mem(buffer);
        buffer = o.buffer;
        o.buffer = nullptr;
    }
    return *this;
}

hscfs_block_buffer::~hscfs_block_buffer()
{
    comm_free_dma_mem(buffer);
}
