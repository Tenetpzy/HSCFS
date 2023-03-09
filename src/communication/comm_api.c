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
        HSCFS_LOG(HSCFS_LOG_ERROR, "async rw: alloc session ctx failed.");
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
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "async rw: send read cmd failed.");
        goto err2;
    }

    ret = comm_session_submit_cmd_ctx(session_ctx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "async rw: submit ctx to session failed.");
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
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync rw: construct session ctx failed.");
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
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync rw: send read cmd failed.");
        goto err1;
    }

    ret = comm_session_submit_cmd_ctx(&session_ctx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync rw: submit ctx to session failed.");
        goto err1;
    }

    ret = mutex_lock(&session_ctx.wait_mtx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync rw: lock session ctx failed.");
        goto err1;
    }
    while (!session_ctx.cmd_is_cplt)
    {
        ret = cond_wait(&session_ctx.wait_cond, &session_ctx.wait_mtx);
        if (ret != 0)
        {
            HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "sync rw: wait session failed.");
            goto err2;
        }
    }

    if (session_ctx.cmd_result != COMM_CMD_SUCCESS)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "sync rw: cmd execute failed.");
        ret = -1;
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

    if (session_ctx.cmd_result != COMM_CMD_SUCCESS)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "sync raw cmd: cmd execute failed.");
        ret = -1;
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

int comm_submit_sync_migrate_request(comm_dev *dev, migrate_task *task)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword10 = sizeof(migrate_task) / 4;
    cmd.dword12 = 0x10021;
    // dword13由comm_raw_sync_cmd_sender设置
    int ret = comm_raw_sync_cmd_sender(dev, task, sizeof(migrate_task), &cmd, 1, NULL, 0);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit sync migrate request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_async_migrate_request(comm_dev *dev, migrate_task *task, comm_async_cb_func cb_func, void *cb_arg)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword10 = sizeof(migrate_task) / 4;
    cmd.dword12 = 0x10021;
    int ret = comm_raw_async_cmd_sender(dev, task, sizeof(migrate_task), &cmd, 1, NULL, 0, cb_func, cb_arg);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit async migrate request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_sync_path_lookup_request(comm_dev *dev, path_lookup_task *task, path_lookup_result *res)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword10 = sizeof(path_lookup_task) / 4;
    cmd.dword12 = 0x20021;
    int ret = comm_raw_sync_cmd_sender(dev, task, sizeof(path_lookup_task), &cmd, 1, res, sizeof(path_lookup_result));
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit sync path lookup request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_async_path_lookup_request(comm_dev *dev, path_lookup_task *task, path_lookup_result *res, 
    comm_async_cb_func cb_func, void *cb_arg)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword10 = sizeof(path_lookup_task) / 4;
    cmd.dword12 = 0x20021;
    int ret = comm_raw_async_cmd_sender(dev, task, sizeof(path_lookup_task), &cmd, 1, 
        res, sizeof(path_lookup_result), cb_func, cb_arg);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit async path lookup request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_sync_filemapping_search_request(comm_dev *dev, filemapping_search_task *task, 
    void *res, uint32_t res_len)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword10 = sizeof(filemapping_search_task) / 4;
    cmd.dword12 = 0x30021;
    int ret = comm_raw_sync_cmd_sender(dev, task, sizeof(filemapping_search_task), &cmd, 1, res, res_len);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit sync filemapping search request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_async_filemapping_search_request(comm_dev *dev, filemapping_search_task *task, 
    void *res, uint32_t res_len, comm_async_cb_func cb_func, void *cb_arg)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword10 = sizeof(filemapping_search_task) / 4;
    cmd.dword12 = 0x30021;
    int ret = comm_raw_async_cmd_sender(dev, task, sizeof(filemapping_search_task), &cmd, 1, res, res_len,
        cb_func, cb_arg);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit async filemapping search request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_sync_update_metajournal_tail_request(comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword12 = 0x40021;
    cmd.dword13 = origin_lpa >> 32;
    cmd.dword14 = origin_lpa & (UINT32_MAX);
    cmd.dword15 = write_block_num;
    int ret = comm_raw_sync_cmd_sender(dev, NULL, 0, &cmd, 0, NULL, 0);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit sync update metajournal tail request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_async_update_metajournal_tail_request(comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num,
    comm_async_cb_func cb_func, void *cb_arg)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword12 = 0x40021;
    cmd.dword13 = origin_lpa >> 32;
    cmd.dword14 = origin_lpa & (UINT32_MAX);
    cmd.dword15 = write_block_num;
    int ret = comm_raw_async_cmd_sender(dev, NULL, 0, &cmd, 0, NULL, 0, cb_func, cb_arg);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit async update metajournal tail request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_fs_module_init_request(comm_dev *dev)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword12 = 0x80021;
    int ret = comm_raw_sync_cmd_sender(dev, NULL, 0, &cmd, 0, NULL, 0);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit fs module init request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_fs_db_init_request(comm_dev *dev)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword12 = 0x90021;
    int ret = comm_raw_sync_cmd_sender(dev, NULL, 0, &cmd, 0, NULL, 0);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit fs db init request failed.");
        return ret;
    }
    return 0;   
}

int comm_submit_fs_recover_from_db_request(comm_dev *dev)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword12 = 0xA0021;
    int ret = comm_raw_sync_cmd_sender(dev, NULL, 0, &cmd, 0, NULL, 0);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit fs recover from db request failed.");
        return ret;
    }
    return 0;   
}

int comm_submit_clear_metajournal_request(comm_dev *dev)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword12 = 0xB0021;
    int ret = comm_raw_sync_cmd_sender(dev, NULL, 0, &cmd, 0, NULL, 0);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit clear metajournal request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_start_apply_journal_request(comm_dev *dev)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword12 = 0xC0021;
    int ret = comm_raw_sync_cmd_sender(dev, NULL, 0, &cmd, 0, NULL, 0);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit start apply journal request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_stop_apply_journal_request(comm_dev *dev)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_SET_OPCODE;
    cmd.dword12 = 0xD0021;
    int ret = comm_raw_sync_cmd_sender(dev, NULL, 0, &cmd, 0, NULL, 0);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit stop apply journal request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_sync_get_metajournal_head_request(comm_dev *dev, uint64_t *head_lpa)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_GET_OPCODE;
    cmd.dword10 = 2;
    cmd.dword12 = 0x70021;
    int ret = comm_raw_sync_cmd_sender(dev, head_lpa, 8, &cmd, 0, NULL, 0);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit sync get metajournal head request failed.");
        return ret;
    }
    return 0;
}

int comm_submit_async_get_metajournal_head_request(comm_dev *dev, uint64_t *head_lpa, 
    comm_async_cb_func cb_func, void *cb_arg)
{
    comm_raw_cmd cmd = {0};
    cmd.opcode = VENDOR_GET_OPCODE;
    cmd.dword10 = 2;
    cmd.dword12 = 0x70021;
    int ret = comm_raw_async_cmd_sender(dev, head_lpa, 8, &cmd, 0, NULL, 0, cb_func, cb_arg);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "submit async get metajournal head request failed.");
        return ret;
    }
    return 0;
}