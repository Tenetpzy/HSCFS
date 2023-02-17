#pragma once

#include "communication/comm_api.h"
#include "communication/channel.h"

#include <stdint.h>

typedef struct comm_session_cmd_ctx comm_session_cmd_ctx;
typedef enum comm_session_cmd_nvme_type {
    SESSION_ADMIN_CMD, SESSION_IO_CMD
} comm_session_cmd_nvme_type;

/************************************************************/
/* 会话层命令上下文（comm_session_cmd_ctx）控制 */

/* 
会话层命令上下文的创建接口：
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

// 释放命令上下文同步相关的资源
void free_comm_session_cmd_ctx_sync_info(comm_session_cmd_ctx *self);

// 释放命令上下文结构
// 不负责释放channelchannel
void free_comm_session_cmd_ctx(comm_session_cmd_ctx *self);

/****************************************************************************/

/* 会话层环境与轮询线程接口 */

// 会话层环境初始化，需要在系统启动时调用
int comm_session_env_init(void);

// 将命令上下文提交给会话层，此后由轮询线程完成轮询该命令CQE等后续流程
int comm_session_env_submit_cmd_ctx(comm_session_cmd_ctx *cmd_ctx);

// 接口层向信道层提交命令时，使用的回调函数
void comm_session_polling_thread_callback(comm_cmd_CQE_result result, void *arg);