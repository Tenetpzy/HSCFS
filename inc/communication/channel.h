#pragma once

#include <pthread.h>
#include <sys/queue.h>
#include <stdint.h>

#include "spdk/nvme.h"
#include "communication/dev.h"

/* channel操作接口 */
struct comm_channel;

// channel句柄，用户使用此对象控制对应channel
typedef struct comm_channel* comm_channel_handle;

// 信道层调用上层回调时，传递的CQE状态参数
typedef enum comm_cmd_CQE_result
{
    CMD_CQE_SUCCESS, CMD_CQE_ERROR
} comm_cmd_CQE_result;

/*
channel_cmd_cb_func：使用channel发送命令时设置的回调函数。
若该命令已成功发送，此函数在轮询channel时被调用。
result表示命令的CQE结果。
arg是发送命令时传入的回调参数。
*/
typedef void(*channel_cmd_cb_func)(comm_cmd_CQE_result result, void *arg);

// 自定义命令的内容，由上层构造并传递给channel
typedef struct comm_raw_cmd
{
    uint16_t opcode;
    uint32_t dword10;
    uint32_t dword12;
    uint32_t dword13;
    uint32_t dword14;
    uint32_t dword15;
    uint8_t valid_bitmap;   // 暂时不用，spdk raw接口不检查有效性，无效字段可以为任意值。
} comm_raw_cmd;

#define RAW_CMD_DWORD10_VALID 1U
#define RAW_CMD_DWORD12_VALID (1U << 1)
#define RAW_CMD_DWORD13_VALID (1U << 2)
#define RAW_CMD_DWORD14_VALID (1U << 3)
#define RAW_CMD_DWORD15_VALID (1U << 4)

// 锁定一个channel，返回0成功，否则返回对应errno。
int comm_channel_lock(comm_channel_handle self);

/*
尝试锁定一个channel。
返回0为成功锁定；返回EBUSY为channel已被锁定；其它错误则返回对应errno。
*/
int comm_channel_trylock(comm_channel_handle self);

// 解锁一个channel
void comm_channel_unlock(comm_channel_handle self);

// 通过handle发送read命令。返回0成功，否则返回对应errno。
int comm_channel_send_read_cmd_no_lock(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    channel_cmd_cb_func cb_func, void *cb_arg);
int comm_channel_send_read_cmd(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    channel_cmd_cb_func cb_func, void *cb_arg);

// 通过handle发送write命令。返回0成功，否则返回对应errno。
int comm_channel_send_write_cmd_no_lock(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    channel_cmd_cb_func cb_func, void *cb_arg);
int comm_channel_send_write_cmd(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    channel_cmd_cb_func cb_func, void *cb_arg);

/* 
发送自定义命令。
上层负责构造comm_raw_cmd中的命令相关字段。
返回0成功，否则返回对应errno。
此接口线程安全。
*/
int comm_send_raw_cmd(comm_dev *dev, void *buf, uint32_t buf_len, comm_raw_cmd *raw_cmd, 
    channel_cmd_cb_func cb_func, void *cb_arg);

/*
轮询handle对应channel中发送的命令是否已经完成。
max_cplt指定此次调用最多处理已完成命令的个数，若为0则处理全部已完成命令。
若某个命令已完成，则此函数内调用发送命令时指定的回调。
调用前需保证已经对handle加锁。

返回处理的已完成命令个数，或-ENXIO，表示底层传输出错。
*/
int comm_channel_polling_completions_no_lock(comm_channel_handle handle, uint32_t max_cplt);

// 轮询管理类命令。
int comm_polling_admin_completions(comm_dev *dev);

/*********************************************************************************/
/* 信道层channel管理器 */

typedef struct comm_channel_controller
{
    struct comm_channel *channels;  // 指向分配的channel数组
    size_t *channel_use_cnt;  // 记录每一个channel当前的使用计数
    size_t _channel_num;  // 当前分配的channel数量
    pthread_spinlock_t lock;  // 用于分配channel时的互斥
} comm_channel_controller;

/* 
comm_channel_controller构造函数
channel_num：需要分配的channel数量
分配channels数组，并构造数组中每一个channel。
返回0成功，否则返回对应errno。
*/
int comm_channel_controller_constructor(comm_channel_controller *self, comm_dev *dev, size_t channel_num);

// 析构函数：析构每一个channel，然后释放channels数组
void comm_channel_controller_destructor(comm_channel_controller *self);

// 获取一个channel，返回该channel的句柄
comm_channel_handle comm_channel_controller_get_channel(comm_channel_controller *self);

// 释放一个channel
void comm_channel_controller_put_channel(comm_channel_controller *self, comm_channel_handle handle);

/*****************************************************************************/
