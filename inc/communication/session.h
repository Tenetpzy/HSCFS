#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "communication/comm_api.h"
#include "communication/dev.h"
#include "utils/queue_extras.h"
#include "utils/hscfs_multithread.h"

#include <stdint.h>

typedef enum comm_session_cmd_nvme_type {
    SESSION_ADMIN_CMD, SESSION_IO_CMD
} comm_session_cmd_nvme_type;

typedef enum comm_session_cmd_sync_attr {
    SESSION_SYNC_CMD, SESSION_ASYNC_CMD
} comm_session_cmd_sync_attr;

// 在轮询线程中处理的命令状态
typedef enum comm_session_cmd_state {
    SESSION_CMD_NEED_POLLING,  // 需要进行轮询
    SESSION_CMD_RECEIVED_CQE,  // 由通信层下发，且已经收到CQE
    SESSION_CMD_TID_WAIT_QUERY, // 需要主动查询结果
    SESSION_CMD_TID_CAN_QUERY, // 可以查询结果
    SESSION_CMD_TID_CPLT_QUERY // 已经查询到结果
} comm_session_cmd_state;

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
            uint8_t has_ownership;
        };

        // 同步接口进行等待的条件变量信息
        struct
        {
            uint8_t cmd_is_cplt;  // 标志该命令是否已在会话层处理完
            cond_t wait_cond;  // 等待会话层处理的条件变量
            mutex_t wait_mtx;  // 与wait_cond和wait_state相关的锁
        };
    };

    uint8_t is_long_cmd;  // 是否属于长命令
    uint16_t tid;  // 长命令的tid
    void *tid_result_buffer;  // 用于保存长命令结果的buffer
    uint32_t tid_result_buf_len;  // result_buffer的长度

    comm_session_cmd_nvme_type cmd_nvme_type;  // 记录该命令是admin还是I/O命令
    comm_session_cmd_state cmd_session_state;  // 由会话层使用的命令状态
    TAILQ_ENTRY(comm_session_cmd_ctx) COMM_SESSION_CMD_QUEUE_FIELD;  // 会话层维护ctx的链表
} comm_session_cmd_ctx;

/************************************************************/
/* 会话层命令上下文（comm_session_cmd_ctx）控制 */

/* 
 * 会话层命令上下文的构造函数：
 * 若成功，返回0，否则返回对应errno。
 * 对于非长命令，需要提供cmd_type参数，指示该命令是admin还是I/O。长命令一定是admin命令，所以不用提供该参数。
 * 对于异步命令，take_ownership若为1，则轮询线程获取self的所有权，轮询线程将在命令执行结束后释放资源。
 * channel参数为发送命令使用的channel。
 * 
 * 通信层发送命令后，可构造会话层命令上下文，并使用comm_session_submit_cmd_ctx将其提交给会话层
 */

// 同步命令
int comm_session_sync_cmd_ctx_constructor(comm_session_cmd_ctx *self, comm_channel_handle channel,
     comm_session_cmd_nvme_type cmd_type);

// 异步命令
int comm_session_async_cmd_ctx_constructor(comm_session_cmd_ctx *self, comm_channel_handle channel, 
    comm_session_cmd_nvme_type cmd_type, comm_async_cb_func cb_func, void *cb_arg, uint8_t take_ownership);

// 同步长命令
int comm_session_sync_long_cmd_ctx_constructor(comm_session_cmd_ctx *self, comm_channel_handle channel, 
    void *tid_res_buf, uint32_t tid_res_len);

// 异步长命令
int comm_session_async_long_cmd_ctx_constructor(comm_session_cmd_ctx *self, comm_channel_handle channel,
    void *tid_res_buf, uint32_t tid_res_len, comm_async_cb_func cb_func, void *cb_arg, uint8_t take_ownership);

// 释放命令上下文同步相关的资源
void comm_session_cmd_ctx_destructor(comm_session_cmd_ctx *self);

// 释放命令上下文结构
// 不负责释放channel
// void free_comm_session_cmd_ctx(comm_session_cmd_ctx *self);

/****************************************************************************/

/* 会话层环境与轮询线程接口 */

// 会话层环境初始化，需要在系统启动时调用
int comm_session_env_init(void);

// 将命令上下文提交给会话层，此后由轮询线程完成轮询该命令CQE等后续流程
int comm_session_submit_cmd_ctx(comm_session_cmd_ctx *cmd_ctx);

// 接口层向信道层提交命令时，使用的回调函数
void comm_session_polling_thread_callback(comm_cmd_CQE_result result, void *arg);

// 轮询线程启动参数
typedef struct polling_thread_start_env
{
    comm_dev *dev;
} polling_thread_start_env;

// 轮询线程入口
void* comm_session_polling_thread(void *arg);

#ifdef __cplusplus
}
#endif