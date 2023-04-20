#pragma once

#include "communication/memory.h"

namespace hscfs {

/* 上层模块使用智能指针管理DMA内存时，可使用此删除器 */
template <typename T>
class dma_buf_deletor
{
public:
    void operator()(T *buf)
    {
        comm_free_dma_mem(buf);
    }
};

}