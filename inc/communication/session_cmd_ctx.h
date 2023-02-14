#pragma once

#include <pthread.h>
#include <stdint.h>
#include <sys/queue.h>

#include "communication/comm_api.h"
#include "communication/channel.h"

typedef enum comm_session_cmd_nvme_type {
    SESSION_ADMIN_CMD, SESSION_IO_CMD
} comm_session_cmd_nvme_type;

typedef enum comm_session_cmd_sync_attr {
    SESSION_SYNC_CMD, SESSION_ASYNC_CMD
} comm_session_cmd_sync_attr;

#define COMM_SESSION_CMD_QUEUE_FIELD queue_entry

// 会话层用于跟踪命令执行的上下文
typedef struct comm_session_cmd_ctx
{
    comm_channel_handle channel;  // 命令使用的channel
    comm_cmd_result cmd_result;  // 命令执行结果状态

    comm_session_cmd_sync_attr cmd_sync_type;  // 标志该命令是同步还是异步
    union
    {
        // 异步接口传入的回调函数和参数
        struct
        {
            comm_async_cb_func async_cb_func;
            void *async_cb_arg;
        };

        // 同步接口进行等待的条件变量信息
        struct
        {
            uint8_t cmd_is_cplt;  // 标志该命令是否已在会话层处理完
            pthread_cond_t wait_cond;  // 等待会话层处理的条件变量
            pthread_mutex_t wait_mtx;  // 与wait_cond和wait_state相关的锁
        };
    };
    
    uint8_t is_long_cmd;  // 是否属于长命令
    uint16_t tid;  // 长命令的tid
    void *tid_result_buffer;  // 用于保存长命令结果的buffer
    uint32_t tid_result_buf_len;  // result_buffer的长度

    comm_session_cmd_nvme_type cmd_nvme_type;  // 记录该命令是admin还是I/O命令
    uint8_t cmd_is_received_CQE;  // 由会话层使用的命令状态，标志CQE是否已收到
    TAILQ_ENTRY(comm_session_cmd_ctx) COMM_SESSION_CMD_QUEUE_FIELD;  // 会话层维护ctx的链表
} comm_session_cmd_ctx;


/************************************************************/
/* 会话层命令上下文（comm_session_cmd_ctx）控制 */

/* 
会话层命令上下文的创建接口。
返回分配的上下文，若返回NULL且rc非空，则将错误码保存在参数rc中。
调用者提供channel句柄，随即该channel托管给会话层命令上下文，不需要调用者释放channel
对于非长命令，需要提供cmd_type参数，指示该命令是admin还是I/O。长命令一定是admin命令，所以不用提供该参数。
*/
// 同步命令
comm_session_cmd_ctx* new_comm_session_sync_cmd_ctx(comm_channel_handle channel,
     comm_session_cmd_nvme_type cmd_type, int *rc);

// 异步命令
comm_session_cmd_ctx* new_comm_session_async_cmd_ctx(comm_channel_handle channel, 
    comm_session_cmd_nvme_type cmd_type, comm_async_cb_func cb_func, void *cb_arg, int *rc);

// 同步长命令
comm_session_cmd_ctx* new_comm_session_sync_long_cmd_ctx(comm_channel_handle channel,
    void *tid_res_buf, uint32_t tid_res_len, int *rc);

// 异步长命令
comm_session_cmd_ctx* new_comm_session_async_long_cmd_ctx(comm_channel_handle channel,
    void *tid_res_buf, uint32_t tid_res_len, comm_async_cb_func cb_func, void *cb_arg, int *rc);

// 释放会话层命令上下文，通信层申请的channel在此处释放
void free_comm_session_cmd_ctx(comm_session_cmd_ctx *self);

static inline comm_session_cmd_nvme_type comm_session_cmd_ctx_get_nvme_type(comm_session_cmd_ctx *self)
{
    return self->cmd_nvme_type;
}

/*********************************************************************************/