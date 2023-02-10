#include <stdlib.h>
#include <error.h>
#include "communication/channel.h"
#include "utils/sys_log.h"

comm_channel_env* comm_channel_env_get_instance()
{
    static comm_channel_env g_channel_env;
    return &g_channel_env;
}

int comm_channel_constructor(comm_channel *self)
{
    self->qpair = spdk_nvme_ctrlr_alloc_io_qpair(comm_channel_env_get_instance()->ctrlr, NULL, 0);
    if (self->qpair == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc I/O queue pair failed!");
        return 1;
    }
    int ret = pthread_mutex_init(&self->lock, NULL);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "initialize channel lock failed.");
        spdk_nvme_ctrlr_free_io_qpair(self->qpair);
        return ret;
    }
    return 0;
}

void comm_channel_destructor(comm_channel *self)
{
    spdk_nvme_ctrlr_free_io_qpair(self->qpair);
    int ret = pthread_mutex_destroy(&self->lock);
    if (ret != 0)
        HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, ret, "free channel lock error.");
}

comm_channel_controller* comm_channel_controller_get_instance()
{
    static comm_channel_controller g_channel_ctrlr;
    return &g_channel_ctrlr;
}

int comm_channel_controller_constructor(comm_channel_controller *self, size_t channel_num)
{
    int ret;
    self->channels = malloc(sizeof(comm_channel) * channel_num);
    if (self->channels == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc channels failed!");
        return 1;
    }
    size_t init_cnt = 0;
    for (; init_cnt < channel_num; ++init_cnt)
    {
        if (comm_channel_constructor(self->channels + init_cnt) != 0)
            goto err1;
    }

    ret = pthread_spin_init(&self->lock, PTHREAD_PROCESS_PRIVATE);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "init channel controller lock failed!");
        goto err1;
    }

    self->channel_use_cnt = malloc(channel_num * sizeof(size_t));
    if (self->channel_use_cnt == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc channel_use_cnt failed!");
        goto err2;
    }
    for (size_t i = 0; i < channel_num; ++i)
        atomic_init(&self->channel_use_cnt[i], 0);

    self->_channel_num = channel_num;
    return 0;

    // 销毁controller锁
    err2:
    ret = pthread_spin_destroy(&self->lock);
    if (ret != 0)
        HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, ret, "free channel controller lock error.");

    // 释放已构造的所有comm_channel的资源，然后释放channels数组
    err1:
    for (size_t i = 0; i < init_cnt; ++i)
        comm_channel_destructor(self->channels + i);
    free(self->channels);
    return 1;
}

void comm_channel_controller_destructor(comm_channel_controller *self)
{
    for (size_t i = 0; i < self->_channel_num; ++i)
        comm_channel_destructor(&self->channels[i]);
    free(self->channels);
    free(self->channel_use_cnt);
    int ret = pthread_spin_destroy(&self->lock);
    if (ret != 0)
        HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, ret, "free channel controller lock error.");
}

comm_channel_handle comm_channel_controller_get_channel(comm_channel_controller *self)
{
    pthread_spin_lock(&self->lock);
    size_t min_use = SIZE_MAX;
    comm_channel_handle min_channel = 0;
    
    // 选择当前使用计数最小的channel
    for (size_t i = 0; i < self->_channel_num; ++i)
    {
        size_t cur_use = self->channel_use_cnt[i];
        if (cur_use < min_use)
        {
            min_use = cur_use;
            min_channel = (comm_channel_handle)i;
        }
    }
    ++self->channel_use_cnt[min_channel];
    pthread_spin_unlock(&self->lock);
    return min_channel;
}

void comm_channel_controller_put_channel(comm_channel_controller *self, comm_channel_handle handle)
{
    pthread_spin_lock(&self->lock);
    --self->channel_use_cnt[handle];
    pthread_spin_unlock(&self->lock);
}

int comm_channel_controller_lock_channel(comm_channel_controller *self, comm_channel_handle handle)
{
    int ret = pthread_mutex_lock(&self->channels[handle].lock);
    if (ret != 0)
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "lock channel failed.");
    return ret;
}

// 可用于不可阻塞的轮询线程
int comm_channel_controller_trylock_channel(comm_channel_controller *self, comm_channel_handle handle)
{
    int ret = pthread_mutex_trylock(&self->channels[handle].lock);
    if (ret == EBUSY)
        return 1;
    else if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "trylock channel failed.");
        return -1;
    }
    return 0;
}

void comm_channel_controller_unlock_channel(comm_channel_controller *self, comm_channel_handle handle)
{
    pthread_mutex_unlock(&self->channels[handle].lock);
}


typedef struct channel_cmd_cb_ctx
{
    cmd_cb_func caller_cb_func;
    void *caller_cb_arg;
} channel_cmd_cb_ctx;

static void channel_inner_spdk_cmd_callback(void *ctx, const struct spdk_nvme_cpl *cpl)
{
    channel_cmd_cb_ctx *cmd_cb_ctx = (channel_cmd_cb_ctx *)ctx;
    CQE_status status = CQE_SUCCESS;

    // CQE为错误，打印日志并设置status
    if (spdk_nvme_cpl_is_error(cpl))  
    {
        HSCFS_LOG(HSCFS_LOG_WARNING, "cmd CQE error, error status: %s", 
            spdk_nvme_cpl_get_status_string(&cpl->status));
        status = CQE_ERROR;
    }

    cmd_cb_ctx->caller_cb_func(status, cmd_cb_ctx->caller_cb_arg);
    free(cmd_cb_ctx);
}

// 应该提供加锁的/不加锁的两种接口（轮询线程不应在锁上等待，所以它只进行尝试加锁，成功后再操作，失败则等下一轮）
int comm_channel_send_read_cmd(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    cmd_cb_func cb_func, void *cb_arg)
{
    int err_ret;
    channel_cmd_cb_ctx *cmd_cb_ctx = malloc(sizeof(channel_cmd_cb_ctx));
    if (cmd_cb_ctx == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc channel cmd callback ctx failed.");
        return ENOMEM;
    }
    cmd_cb_ctx->caller_cb_func = cb_func;
    cmd_cb_ctx->caller_cb_arg = cb_arg;

    comm_channel_controller *channel_ctrlr = comm_channel_controller_get_instance();
    err_ret = comm_channel_controller_lock_channel(channel_ctrlr, handle);
    if (err_ret != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "send read cmd: lock channel failed.");
        goto err1;
    }

    comm_channel_env *g_env = comm_channel_env_get_instance();
    err_ret = spdk_nvme_ns_cmd_read(g_env->ns, channel_ctrlr->channels[handle].qpair, buffer, lba, lba_count, 
        channel_inner_spdk_cmd_callback, cmd_cb_ctx, 0);
    if (err_ret != 0)
        goto err2;

    comm_channel_controller_unlock_channel(channel_ctrlr, handle);
    return 0;

    // 释放channel锁
    err2:
    comm_channel_controller_unlock_channel(channel_ctrlr, handle);

    // 释放分配的cmd_cb_ctx
    err1:
    free(cmd_cb_ctx);

    return err_ret;
}