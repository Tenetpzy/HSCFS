#include "cache/block_buffer.hh"
#include "communication/memory.h"
#include "utils/hscfs_exceptions.hh"
#include <cstring>

namespace hscfs {

block_buffer::block_buffer()
{
    buffer = static_cast<char*>(comm_alloc_dma_mem(4096));
    if (buffer == nullptr)
        throw alloc_error("alloc block buffer failed.");
}

block_buffer::block_buffer(const block_buffer &o): block_buffer()
{
    std::memcpy(buffer, o.buffer, 4096);
}

block_buffer::block_buffer(block_buffer &&o) noexcept
{
    if (this != &o)
    {
        buffer = o.buffer;
        o.buffer = nullptr;
    }
}

block_buffer &block_buffer::operator=(const block_buffer &o)
{
    if (this != &o)
        std::memcpy(buffer, o.buffer, 4096);
    return *this;
}

block_buffer &block_buffer::operator=(block_buffer &&o) noexcept
{
    if (this != &o)
    {
        comm_free_dma_mem(buffer);
        buffer = o.buffer;
        o.buffer = nullptr;
    }
    return *this;
}

void block_buffer::copy_content_from_buf(char *buf)
{
    std::memcpy(buffer, buf, 4096);
}

block_buffer::~block_buffer()
{
    comm_free_dma_mem(buffer);
}

}