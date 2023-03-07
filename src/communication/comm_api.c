#include <stdlib.h>
#include <errno.h>

#include "communication/comm_api.h"
#include "communication/dev.h"
#include "communication/session.h"
#include "utils/hscfs_log.h"

int comm_submit_async_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count,
    comm_async_cb_func cb_func, void *cb_arg, comm_io_direction dir)
{
    int ret = 0;
    comm_channel_handle channel = comm_channel_controller_get_channel(&dev->channel_ctrlr);
    comm_session_cmd_ctx *session_ctx = (comm_session_cmd_ctx *)malloc(sizeof(comm_session_cmd_ctx));
    if (session_ctx == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "async read: alloc session ctx failed.");
        ret = ENOMEM;
        goto err1;
    }
    comm_session_async_cmd_ctx_constructor(session_ctx, channel, SESSION_IO_CMD, 
        cb_func, cb_arg, 1);  // 初始化异步命令不会出错

    if (dir == COMM_IO_READ)
    {
        ret = comm_channel_send_read_cmd(channel, buffer, lba, lba_count, 
            comm_session_polling_thread_callback, session_ctx);
    }
    else
    {
        ret = comm_channel_send_write_cmd(channel, buffer, lba, lba_count, 
            comm_session_polling_thread_callback, session_ctx); 
    }

    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "async read: send read cmd failed.");
        goto err2;
    }

    ret = comm_session_submit_cmd_ctx(session_ctx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "async read: submit ctx to session failed.");
        // 此处已经成功下发命令，而没有轮询，可能导致channel对应qpair资源始终被占用
        // 但发生此错误应是panic的，所以不做轮询处理
        goto err2;
    }

    return 0;

    err2:
    comm_session_cmd_ctx_destructor(session_ctx);
    free(session_ctx);
    err1:
    comm_channel_release(channel);
    return ret;
}

int comm_submit_sync_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count, 
    comm_io_direction dir)
{
    int ret = 0;
    comm_channel_handle channel = comm_channel_controller_get_channel(&dev->channel_ctrlr);
    comm_session_cmd_ctx session_ctx;
    ret = comm_session_sync_cmd_ctx_constructor(&session_ctx, channel, SESSION_IO_CMD);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync read: construct session ctx failed.");
        goto err0;
    }

    if (dir == COMM_IO_READ)
    {
        ret = comm_channel_send_read_cmd(channel, buffer, lba, lba_count, 
            comm_session_polling_thread_callback, &session_ctx);
    }
    else
    {
        ret = comm_channel_send_write_cmd(channel, buffer, lba, lba_count, 
            comm_session_polling_thread_callback, &session_ctx);
    }
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync read: send read cmd failed.");
        goto err1;
    }

    ret = comm_session_submit_cmd_ctx(&session_ctx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync read: submit ctx to session failed.");
        goto err1;
    }

    ret = mutex_lock(&session_ctx.wait_mtx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync read: lock session ctx failed.");
        goto err1;
    }
    while (!session_ctx.cmd_is_cplt)
    {
        ret = cond_wait(&session_ctx.wait_cond, &session_ctx.wait_mtx);
        if (ret != 0)
        {
            HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync read: wait session failed.");
            goto err2;
        }
    }

    err2:
    mutex_unlock(&session_ctx.wait_mtx);
    err1:
    comm_session_cmd_ctx_destructor(&session_ctx);
    err0:
    comm_channel_release(channel);
    return ret;
}

int comm_raw_sync_cmd_sender(comm_dev *dev, void *buf, uint32_t buf_len, comm_raw_cmd *raw_cmd,
    uint8_t is_long_cmd, void *tid_res_buf, uint32_t tid_res_len)
{
    int ret = 0;
    comm_channel_handle channel = comm_channel_controller_get_channel(&dev->channel_ctrlr);

    comm_session_cmd_ctx session_ctx;
    if (is_long_cmd)
        ret = comm_session_sync_long_cmd_ctx_constructor(&session_ctx, channel, tid_res_buf, tid_res_len);
    else
        ret = comm_session_sync_cmd_ctx_constructor(&session_ctx, channel, SESSION_ADMIN_CMD);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync raw cmd: construct session ctx failed.");
        goto err1;
    }

    if (is_long_cmd)
        raw_cmd->dword13 = session_ctx.tid;
    
    ret = comm_send_raw_cmd(channel, buf, buf_len, raw_cmd, comm_session_polling_thread_callback, &session_ctx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync raw cmd: send cmd failed.");
        goto err2;
    }

    ret = comm_session_submit_cmd_ctx(&session_ctx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync raw cmd: submit ctx to session failed.");
        // panic错误，不考虑下发成功但不轮询可能导致的channel资源泄露
        goto err2;
    }

    ret = mutex_lock(&session_ctx.wait_mtx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync raw cmd: lock session ctx failed.");
        goto err2;
    }
    while (!session_ctx.cmd_is_cplt)
    {
        ret = cond_wait(&session_ctx.wait_cond, &session_ctx.wait_mtx);
        if (ret != 0)
        {
            HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync raw cmd: wait session failed.");
            goto err3;
        }
    }

    err3:
    mutex_unlock(&session_ctx.wait_mtx);
    err2:
    comm_session_cmd_ctx_destructor(&session_ctx);
    err1:
    comm_channel_release(channel);
    return ret;
}

int comm_raw_async_cmd_sender(comm_dev *dev, void *buf, uint32_t buf_len, comm_raw_cmd *raw_cmd,
    uint8_t is_long_cmd, void *tid_res_buf, uint32_t tid_res_len, 
    comm_async_cb_func cb_func, void *cb_arg)
{
    int ret = 0;
    comm_channel_handle channel = comm_channel_controller_get_channel(&dev->channel_ctrlr);
    comm_session_cmd_ctx *session_ctx = (comm_session_cmd_ctx *)malloc(sizeof(comm_session_cmd_ctx));
    if (session_ctx == NULL)
    {
        ret = ENOMEM;
        goto err1;
    }

    if (is_long_cmd)
    {
        ret = comm_session_async_long_cmd_ctx_constructor(session_ctx, channel, tid_res_buf, tid_res_len,
            cb_func, cb_arg, 1);
    }
    else
        ret = comm_session_async_cmd_ctx_constructor(session_ctx, channel, SESSION_ADMIN_CMD, cb_func, cb_arg, 1);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "async raw cmd: construct session ctx failed.");
        goto err2;
    }
    
    // 长命令的命令结构的tid字段由此处（会话层）设置，调用者设置其它字段
    if (is_long_cmd)
        raw_cmd->dword13 = session_ctx->tid;
    
    ret = comm_send_raw_cmd(channel, buf, buf_len, raw_cmd, comm_session_polling_thread_callback, session_ctx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "async raw cmd: send cmd failed.");
        goto err3;
    }

    ret = comm_session_submit_cmd_ctx(session_ctx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "async raw cmd: submit ctx to session failed.");
        // panic错误，不考虑下发成功但不轮询可能导致的channel资源泄露
        goto err3;
    }
    return 0;

    err3:
    comm_session_cmd_ctx_destructor(session_ctx);
    err2:
    free(session_ctx);
    err1:
    comm_channel_release(channel);
    return ret;
}

