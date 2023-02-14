#include "communication/session_cmd_ctx.h"
#include "utils/sys_log.h"

// 初始化同步命令的命令类型(同步/异步标志)、条件变量相关信息
static int comm_session_cmd_ctx_init_sync_info(comm_session_cmd_ctx *ctx)
{
    int ret = 0;
    ret = pthread_cond_init(&ctx->wait_cond, NULL);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "init session cmd ctx condvar failed.");
        return ret;
    }
    ret = pthread_mutex_init(&ctx->wait_mtx, NULL);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "init session cmd ctx condmtx failed.");
        pthread_cond_destroy(&ctx->wait_cond);  // 不会返回error
    }
    ctx->cmd_sync_type = SESSION_SYNC_CMD;
    ctx->cmd_is_cplt = 0;
    return ret;
}

// 初始化异步命令的命令类型(同步/异步标志)，回调相关信息
static inline void comm_session_cmd_ctx_init_async_info(comm_session_cmd_ctx *ctx, 
    comm_async_cb_func cb_func, void *cb_arg)
{
    ctx->cmd_sync_type = SESSION_ASYNC_CMD;
    ctx->async_cb_func = cb_func;
    ctx->async_cb_arg = cb_arg;
}

// 分配新的tid。同时下发多于UINT16_MAX个长命令时会出现tid重复，可能导致错误，暂时不处理。
static uint16_t alloc_new_tid(void)
{
    static uint16_t tid = 0;
    return tid++;
}

// 分配comm_session_cmd_ctx结构体，并初始化属性
static comm_session_cmd_ctx* new_comm_session_cmd_ctx_inner(comm_channel_handle channel,
    comm_session_cmd_nvme_type cmd_nvme_type, comm_session_cmd_sync_attr sync_type, 
    comm_async_cb_func cb_func, void *cb_arg, uint8_t long_cmd, void *res_buf, uint32_t res_len, int *rc)
{
    comm_session_cmd_ctx *ctx = (comm_session_cmd_ctx *)malloc(sizeof(comm_session_cmd_ctx));
    if (ctx == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc comm_session_cmd_ctx failed.");
        *rc = ENOMEM;
        return NULL;
    }

    if (sync_type == SESSION_SYNC_CMD)
    {
        int ret = comm_session_cmd_ctx_init_sync_info(ctx);
        if (ret != 0)
        {
            *rc = ret;
            free(ctx);
            return NULL;
        }
    }
    else
        comm_session_cmd_ctx_init_async_info(ctx, cb_func, cb_arg);

    if (long_cmd)
    {
        ctx->tid = alloc_new_tid();
        ctx->tid_result_buffer = res_buf;
        ctx->tid_result_buf_len = res_len;
    }
    ctx->is_long_cmd = long_cmd;
    
    ctx->channel = channel;
    ctx->cmd_nvme_type = cmd_nvme_type;
    ctx->cmd_is_received_CQE = 0;

    return ctx;
}

comm_session_cmd_ctx* new_comm_session_sync_cmd_ctx(comm_channel_handle channel, 
    comm_session_cmd_nvme_type cmd_type, int *rc)
{
    return new_comm_session_cmd_ctx_inner(channel, cmd_type, SESSION_SYNC_CMD, 
        NULL, NULL, 0, NULL, 0, rc);
}

comm_session_cmd_ctx* new_comm_session_async_cmd_ctx(comm_channel_handle channel,
    comm_session_cmd_nvme_type cmd_type, comm_async_cb_func cb_func, void *cb_arg, int *rc)
{
    return new_comm_session_cmd_ctx_inner(channel, cmd_type, SESSION_ASYNC_CMD, 
        cb_func, cb_arg, 0, NULL, 0, rc);
}

comm_session_cmd_ctx* new_comm_session_sync_long_cmd_ctx(comm_channel_handle channel, 
    void *tid_res_buf, uint32_t tid_res_len, int *rc)
{
    return new_comm_session_cmd_ctx_inner(channel, SESSION_ADMIN_CMD, SESSION_SYNC_CMD,
        NULL, NULL, 1, tid_res_buf, tid_res_len, rc);
}

comm_session_cmd_ctx* new_comm_session_async_long_cmd_ctx(comm_channel_handle channel, 
    void *tid_res_buf, uint32_t tid_res_len, comm_async_cb_func cb_func, void *cb_arg, int *rc)
{
    return new_comm_session_cmd_ctx_inner(channel, SESSION_ADMIN_CMD, SESSION_ASYNC_CMD,
        cb_func, cb_arg, 1, tid_res_buf, tid_res_len, rc);
}

void free_comm_session_cmd_ctx(comm_session_cmd_ctx *self)
{
    if (self->cmd_sync_type == SESSION_SYNC_CMD)
    {
        int ret = pthread_mutex_destroy(&self->wait_mtx);
        if (ret != 0)
            HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, ret, "destroy session cmd ctx wait_mtx failed.");
        ret = pthread_cond_destroy(&self->wait_cond);
        if (ret != 0)
            HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, ret, "destroy session cmd ctx wait_cond failed.");
    }
    comm_channel_release(self->channel);
}