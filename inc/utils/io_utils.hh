#pragma once

#include <cstddef>
#include <atomic>
#include <future>
#include "communication/comm_api.h"

/*
 * 异步向量I/O的同步器
 * 用于需要等待多个非连续缓冲区的异步I/O都完成的情景
 * 不允许多个线程同时等待
 */
class async_vecio_synchronizer
{
public:
    // io_num为该I/O的次数
    async_vecio_synchronizer(uint64_t io_num)
    {
        _io_num = io_num;
        io_res = COMM_CMD_SUCCESS;
    }

    /*
     * 完成一次（一般是一个独立缓存区）I/O时，调用此方法
     * 一般由回调函数调用
     */
    void cplt_once(comm_cmd_result io_result)
    {
        uint64_t num = _io_num.fetch_sub(1);
        if (io_res == COMM_CMD_SUCCESS)
            io_res = io_result;
        if (num == 1)  // _io_num由调用线程减至0，则向量I/O完成，发送通知
            io_cplt.set_value(io_res);
    }

    // 等待该异步向量I/O完成
    comm_cmd_result wait_cplt()
    {
        return io_cplt.get_future().get();
    }

private:
    comm_cmd_result io_res;
    std::atomic_uint64_t _io_num;
    std::promise<comm_cmd_result> io_cplt;
};

#define LPA_TO_LBA(lpa)  ((lpa) * 8)