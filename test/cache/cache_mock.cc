#include <cstdlib>
#include <cstdint>

extern "C" {

struct comm_dev
{
    int unused;
};

typedef enum comm_io_direction
{
    COMM_IO_READ, COMM_IO_WRITE
} comm_io_direction;

int comm_submit_sync_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count, 
    comm_io_direction dir)
{
    return 0;
}

void *spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
{
    return malloc(size);
}

void spdk_free(void *buf)
{
    free(buf);
}

}  // extern "C"