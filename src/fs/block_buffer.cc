#include "fs/block_buffer.hh"
#include "communication/memory.h"
#include "utils/hscfs_exceptions.hh"

hscfs_block_buffer::hscfs_block_buffer()
{
    buffer = static_cast<char*>(comm_alloc_dma_mem(4096));
    if (buffer == nullptr)
        throw hscfs_alloc_error("alloc block buffer failed.");
}

hscfs_block_buffer::~hscfs_block_buffer()
{
    comm_free_dma_mem(buffer);
}


