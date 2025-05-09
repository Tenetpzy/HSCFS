#include "cache/block_buffer.hh"
#include "communication/memory.h"
#include "utils/hscfs_exceptions.hh"
#include "utils/io_utils.hh"
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

void block_buffer::read_from_lpa(comm_dev *dev, uint32_t lpa)
{
    int ret = comm_submit_sync_rw_request(dev, buffer, LPA_TO_LBA(lpa), LBA_PER_LPA, COMM_IO_READ);
    if (ret != 0)
        throw io_error("read lpa failed.");
}

void block_buffer::write_to_lpa_sync(comm_dev *dev, uint32_t lpa)
{
    int ret = comm_submit_sync_rw_request(dev, buffer, LPA_TO_LBA(lpa), LBA_PER_LPA, COMM_IO_WRITE);
    if (ret != 0)
        throw io_error("sync write lpa failed.");
}

void block_buffer::write_to_lpa_async(comm_dev *dev, uint32_t lpa, comm_async_cb_func cb_func, void *cb_arg)
{
    int ret = comm_submit_async_rw_request(dev, buffer, LPA_TO_LBA(lpa), LBA_PER_LPA, cb_func, cb_arg, COMM_IO_WRITE);
    if (ret != 0)
        throw io_error("async write lpa failed.");
}

block_buffer::~block_buffer()
{
    comm_free_dma_mem(buffer);
}

}