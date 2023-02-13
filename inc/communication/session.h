#pragma once

#include <pthread.h>
#include <stdint.h>

#include "communication/comm_api.h"
#include "communication/channel.h"

typedef enum comm_session_cmd_nvme_type {
    SESSION_ADMIN_CMD, SESSION_IO_CMD
} comm_session_cmd_nvme_type;

typedef enum comm_session_cmd_sync_attr {
    SESSION_SYNC_CMD, SESSION_ASYNC_CMD
} comm_session_cmd_sync_attr;

typedef enum comm_session_long_cmd_type {
    SESSION_NOT_LONG_CMD, SESSION_MIGRATE_CMD, SESSION_PATH_LOOKUP_CMD, SESSION_FILE_MAPPING_CMD
} comm_session_long_cmd_type;

// 会话层用于跟踪命令执行的上下文
typedef struct comm_session_cmd_ctx
{
    comm_channel_handle channel;  // 命令使用的channel
    comm_session_cmd_nvme_type cmd_type;  // 记录该命令是admin还是I/O命令
    uint8_t cmd_is_received_CQE;  // 由会话层使用的命令状态，标志CQE是否已收到
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
    comm_session_long_cmd_type long_cmd_type;  // 长命令类型
    uint16_t tid;  // 长命令的tid
    void *result_buffer;  // 用于保存长命令结果的buffer
    uint32_t result_buf_len;  // result_buffer的长度
} comm_session_cmd_ctx;


/************************************************************/
/* 
会话层命令上下文的创建接口
返回分配的上下文，若返回NULL且rc非空，则将错误码保存在参数rc中
对于非长命令，需要提供cmd_type参数，指示该命令是admin还是I/O。长命令一定是admin命令，所以不用提供该参数。
*/

// 同步命令
comm_session_cmd_ctx* new_session_sync_cmd_ctx(comm_session_cmd_nvme_type cmd_type, int *rc);

// 异步命令
comm_session_cmd_ctx* new_session_async_cmd_ctx(comm_session_cmd_nvme_type cmd_type, 
    comm_async_cb_func cb_func, void *cb_arg, int *rc);

// 同步长命令
comm_session_cmd_ctx* new_session_sync_long_cmd_ctx(void *res_buf, uint32_t res_len, int *rc);

// 异步长命令
comm_session_cmd_ctx* new_session_async_long_cmd_ctx(void *res_buf, uint32_t res_len, 
    comm_async_cb_func cb_func, void *cb_arg, int *rc);

/*********************************************************************************/

// 会话层环境，包含I/O和admin等待队列
typedef struct comm_session_env
{
    
};