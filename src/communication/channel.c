#include <stdlib.h>
#include <error.h>

#include "communication/channel.h"
#include "communication/dev.h"
#include "utils/hscfs_log.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvme.h"
#include "utils/queue_extras.h"

// 构造channel：分配qpair，初始化lock，初始化ref_count为0。返回0成功，否则返回对应errno。
static int comm_channel_constructor(comm_channel *self, comm_dev *dev, size_t index)
{
    self->qpair = spdk_nvme_ctrlr_alloc_io_qpair(dev->nvme_ctrlr, NULL, 0);
    if (self->qpair == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc I/O queue pair failed!");
        return ENOMEM;
    }
    int ret = mutex_init(&self->lock);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "initialize channel lock failed.");
        spdk_nvme_ctrlr_free_io_qpair(self->qpair);
        return ret;
    }
    self->dev = dev;
    self->idx = index;
    return 0;
}

// 析构channel：释放qpair，销毁lock
static void comm_channel_destructor(comm_channel *self)
{
    spdk_nvme_ctrlr_free_io_qpair(self->qpair);
    int ret = mutex_destroy(&self->lock);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "free channel lock error.");
}

// 分配控制器中channels数组，并构造数组中每一个channel。若失败，返回对应errno
int comm_channel_controller_constructor(comm_channel_controller *self, comm_dev *dev, size_t channel_num)
{
    int ret;
    self->channels = (comm_channel*)malloc(sizeof(comm_channel) * channel_num);
    if (self->channels == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc channels failed!");
        return ENOMEM;
    }
    size_t init_cnt = 0;
    for (; init_cnt < channel_num; ++init_cnt)
    {
        ret = comm_channel_constructor(&self->channels[init_cnt], dev, init_cnt);
        if (ret != 0)
            goto err1;
    }

    ret = spin_init(&self->lock);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "init channel controller lock failed!");
        goto err1;
    }

    self->channel_use_cnt = (size_t *)calloc(channel_num, sizeof(size_t));
    if (self->channel_use_cnt == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc channel_use_cnt array failed!");
        ret = ENOMEM;
        goto err2;
    }

    self->_channel_num = channel_num;
    return 0;

    // 销毁controller锁
    err2:
    {
    int res = spin_destroy(&self->lock);
    if (res != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, res, "free channel controller lock error.");
    }
    // 释放已构造的所有comm_channel的资源，然后释放channels数组
    err1:
    for (size_t i = 0; i < init_cnt; ++i)
        comm_channel_destructor(self->channels + i);
    free(self->channels);
    return ret;
}

void comm_channel_controller_destructor(comm_channel_controller *self)
{
    // 析构每一个channel，然后释放channels数组
    for (size_t i = 0; i < self->_channel_num; ++i)
        comm_channel_destructor(&self->channels[i]);
    free(self->channels);
    free(self->channel_use_cnt);
    int ret = spin_destroy(&self->lock);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "free channel controller lock error.");
}

comm_channel_handle comm_channel_controller_get_channel(comm_channel_controller *self)
{
    spin_lock(&self->lock);  // 只可能返回deadlock错误，此处不存在死锁，不检查返回值。
    size_t min_use = SIZE_MAX;
    size_t min_channel = 0;
    
    // 选择当前使用计数最小的channel
    for (size_t i = 0; i < self->_channel_num; ++i)
    {
        size_t cur_use = self->channel_use_cnt[i];
        if (cur_use < min_use)
        {
            min_use = cur_use;
            min_channel = i;
        }
    }
    ++self->channel_use_cnt[min_channel];
    spin_unlock(&self->lock);
    return &self->channels[min_channel];
}

void comm_channel_release(comm_channel_handle self)
{
    comm_channel_controller *channel_ctrlr = &self->dev->channel_ctrlr;
    spin_lock(&channel_ctrlr->lock);
    --channel_ctrlr->channel_use_cnt[self->idx];
    spin_unlock(&channel_ctrlr->lock);
}

int comm_channel_lock(comm_channel_handle self)
{
    int ret = mutex_lock(&self->lock);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "lock channel failed.");
    return ret;
}

// 可用于不可阻塞的轮询线程
int comm_channel_trylock(comm_channel_handle self)
{
    int ret = mutex_trylock(&self->lock);
    if (ret != 0 && ret != EBUSY)
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "trylock channel failed.");
    return ret;
}

void comm_channel_unlock(comm_channel_handle self)
{
    int ret = mutex_unlock(&self->lock);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "unlock channel failed.");
}

// 信道层使用的SPDK命令发送的回调参数
// 包装上层调用channel发送命令时提供的回调函数和参数
typedef struct channel_cmd_cb_ctx
{
    channel_cmd_cb_func caller_cb_func;
    void *caller_cb_arg;
} channel_cmd_cb_ctx;

// 信道层使用的SPDK命令发送的回调函数，封装CQE错误处理，并调用上层的回调函数，将CQE状态传入
static void channel_inner_spdk_cmd_callback(void *ctx, const struct spdk_nvme_cpl *cpl)
{
    channel_cmd_cb_ctx *cmd_cb_ctx = (channel_cmd_cb_ctx *)ctx;
    comm_cmd_CQE_result res = CMD_CQE_SUCCESS;

    // CQE为错误，打印日志并设置status
    if (spdk_nvme_cpl_is_error(cpl))  
    {
        HSCFS_LOG(HSCFS_LOG_WARNING, "cmd CQE error, error status: %s", 
            spdk_nvme_cpl_get_status_string(&cpl->status));
        res = CMD_CQE_ERROR;
    }

    cmd_cb_ctx->caller_cb_func(res, cmd_cb_ctx->caller_cb_arg);
    free(cmd_cb_ctx);
}

static channel_cmd_cb_ctx *new_channel_cmd_cb_ctx(channel_cmd_cb_func cb_func, void *cb_arg)
{
    channel_cmd_cb_ctx *cmd_cb_ctx = (channel_cmd_cb_ctx *)malloc(sizeof(channel_cmd_cb_ctx));
    if (cmd_cb_ctx == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc channel cmd callback ctx failed.");
        return NULL;
    }
    cmd_cb_ctx->caller_cb_func = cb_func;
    cmd_cb_ctx->caller_cb_arg = cb_arg;
    return cmd_cb_ctx;
}

int comm_channel_send_read_cmd_no_lock(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    channel_cmd_cb_func cb_func, void *cb_arg)
{
    channel_cmd_cb_ctx *cmd_cb_ctx = new_channel_cmd_cb_ctx(cb_func, cb_arg);
    if (cmd_cb_ctx == NULL)
        return ENOMEM;
    
    int ret = 0;
    ret = spdk_nvme_ns_cmd_read(handle->dev->ns, handle->qpair, buffer, lba, lba_count, 
        channel_inner_spdk_cmd_callback, cmd_cb_ctx, 0);
    if (ret != 0)
    {
        ret = -ret;  // spdk返回负errno，此处将其变换为errno
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "spdk send read cmd failed.");
        free(cmd_cb_ctx);
    }

    return ret;
}

int comm_channel_send_read_cmd(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    channel_cmd_cb_func cb_func, void *cb_arg)
{
    int ret = 0;
    ret = comm_channel_lock(handle);
    if (ret != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "send read cmd: lock channel failed.");
        return ret;
    }
    ret = comm_channel_send_read_cmd_no_lock(handle, buffer, lba, lba_count, cb_func, cb_arg);
    comm_channel_unlock(handle);
    return ret;
}

int comm_channel_send_write_cmd_no_lock(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    channel_cmd_cb_func cb_func, void *cb_arg)
{
    channel_cmd_cb_ctx *cmd_cb_ctx = new_channel_cmd_cb_ctx(cb_func, cb_arg);
    if (cmd_cb_ctx == NULL)
        return ENOMEM;

    int ret = 0;
    ret = spdk_nvme_ns_cmd_write(handle->dev->ns, handle->qpair, buffer, lba, lba_count, 
        channel_inner_spdk_cmd_callback, cmd_cb_ctx, 0);
    if (ret != 0)
    {
        ret = -ret;
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "spdk send write cmd failed.");
        free(cmd_cb_ctx);
    }

    return ret;
}

int comm_channel_send_write_cmd(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    channel_cmd_cb_func cb_func, void *cb_arg)
{
    int ret = 0;
    ret = comm_channel_lock(handle);
    if (ret != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "send write cmd: lock channel failed.");
        return ret;
    }
    ret = comm_channel_send_write_cmd_no_lock(handle, buffer, lba, lba_count, cb_func, cb_arg);
    comm_channel_unlock(handle);
    return ret;
}

static void build_nvme_cmd(struct spdk_nvme_cmd *nvme_cmd, comm_raw_cmd *raw_cmd, comm_dev *dev)
{
    nvme_cmd->opc = raw_cmd->opcode;
    nvme_cmd->nsid = spdk_nvme_ns_get_id(dev->ns);
    nvme_cmd->cdw10 = raw_cmd->dword10;
    nvme_cmd->cdw12 = raw_cmd->dword12;
    nvme_cmd->cdw13 = raw_cmd->dword13;
    nvme_cmd->cdw14 = raw_cmd->dword14;
    nvme_cmd->cdw15 = raw_cmd->dword15;
}

int comm_send_raw_cmd(comm_channel_handle handle, void *buf, uint32_t buf_len, comm_raw_cmd *raw_cmd, 
    channel_cmd_cb_func cb_func, void *cb_arg)
{
    int ret = 0;
    channel_cmd_cb_ctx *cmd_cb_ctx = new_channel_cmd_cb_ctx(cb_func, cb_arg);
    if (cmd_cb_ctx == NULL)
        return ENOMEM;

    struct spdk_nvme_cmd nvme_cmd = {0};
    build_nvme_cmd(&nvme_cmd, raw_cmd, handle->dev);
    
    ret = spdk_nvme_ctrlr_cmd_admin_raw(handle->dev->nvme_ctrlr, &nvme_cmd, buf, buf_len, 
        channel_inner_spdk_cmd_callback, cmd_cb_ctx);
    if (ret != 0)
    {
        ret = -ret;
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "spdk send raw cmd failed.");
        free(cmd_cb_ctx);
    }
    return ret;
}

int comm_channel_polling_completions_no_lock(comm_channel_handle handle, uint32_t max_cplt)
{
    int ret = spdk_nvme_qpair_process_completions(handle->qpair, max_cplt);
    if (ret == -ENXIO)
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, -ret, "spdk polling I/O cmd failed.");
    return ret;
}

int comm_polling_admin_completions(comm_channel_handle handle)
{
    int ret = spdk_nvme_ctrlr_process_admin_completions(handle->dev->nvme_ctrlr);  
    if (ret == -ENXIO)
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, -ret, "spdk polling I/O cmd failed.");
    return ret;
}