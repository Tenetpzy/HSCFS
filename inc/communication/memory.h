#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "utils/hscfs_log.h"

// 用户态
#include "spdk/env.h"

// spdk内存分配接口
void *spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags);
void spdk_free(void *);

__attribute__((unused)) static void *comm_alloc_dma_mem(size_t size)
{
    void *ret = spdk_zmalloc(size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
    if (ret == NULL)
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc dma memory failed.");
    return ret; 
}

__attribute__((unused)) static void comm_free_dma_mem(void *buf)
{
    spdk_free(buf);
}

#ifdef __cplusplus
}
#endif