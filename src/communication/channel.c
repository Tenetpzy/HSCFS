#include <stdlib.h>
#include <error.h>
#include "communication/channel.h"
#include "utils/sys_log.h"
#include "spdk/nvme_spec.h"

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
        return ENOMEM;
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
        return ENOMEM;
    }
    size_t init_cnt = 0;
    for (; init_cnt < channel_num; ++init_cnt)
    {
        ret = comm_channel_constructor(self->channels + init_cnt);
        if (ret != 0)
            goto err1;
    }

    ret = pthread_spin_init(&self->lock, PTHREAD_PROCESS_PRIVATE);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "init channel controller lock failed!");
        goto err1;
    }

    self->channel_use_cnt = calloc(channel_num, sizeof(size_t));
    if (self->channel_use_cnt == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc channel_use_cnt failed!");
        ret = ENOMEM;
        goto err2;
    }

    self->_channel_num = channel_num;
    return 0;

    // 销毁controller锁
    err2:
    int res = pthread_spin_destroy(&self->lock);
    if (res != 0)
        HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, res, "free channel controller lock error.");

    // 释放已构造的所有comm_channel的资源，然后释放channels数组
    err1:
    for (size_t i = 0; i < init_cnt; ++i)
        comm_channel_destructor(self->channels + i);
    free(self->channels);
    return ret;
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
    if (ret != 0 && ret != EBUSY)
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "trylock channel failed.");
    return ret;
}

void comm_channel_controller_unlock_channel(comm_channel_controller *self, comm_channel_handle handle)
{
    pthread_mutex_unlock(&self->channels[handle].lock);
}

// 信道层使用的SPDK命令发送的回调参数
// 包装上层调用channel发送命令时提供的回调函数和参数
typedef struct channel_cmd_cb_ctx
{
    cmd_cb_func caller_cb_func;
    void *caller_cb_arg;
} channel_cmd_cb_ctx;

// 信道层使用的SPDK命令发送的回调函数，封装CQE错误处理，并调用上层的回调函数，将CQE状态传入
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

static channel_cmd_cb_ctx *new_channel_cmd_cb_ctx(cmd_cb_func cb_func, void *cb_arg)
{
    channel_cmd_cb_ctx *cmd_cb_ctx = malloc(sizeof(channel_cmd_cb_ctx));
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
    cmd_cb_func cb_func, void *cb_arg)
{
    channel_cmd_cb_ctx *cmd_cb_ctx = new_channel_cmd_cb_ctx(cb_func, cb_arg);
    if (cmd_cb_ctx == NULL)
        return ENOMEM;
    
    comm_channel_controller *g_channel_ctrlr = comm_channel_controller_get_instance();
    comm_channel_env *g_env = comm_channel_env_get_instance();
    int ret = spdk_nvme_ns_cmd_read(g_env->ns, g_channel_ctrlr->channels[handle].qpair, buffer, lba, lba_count, 
        channel_inner_spdk_cmd_callback, cmd_cb_ctx, 0);
    if (ret != 0)
    {
        ret = -ret;  // spdk返回负errno，此处将其变换为errno
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "spdk send read cmd failed.");
        free(cmd_cb_ctx);
        return ret;
    }
    return 0;
}

int comm_channel_send_read_cmd(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    cmd_cb_func cb_func, void *cb_arg)
{
    int ret = 0;
    comm_channel_controller *channel_ctrlr = comm_channel_controller_get_instance();
    ret = comm_channel_controller_lock_channel(channel_ctrlr, handle);
    if (ret != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "send read cmd: lock channel failed.");
        return ret;
    }
    ret = comm_channel_send_read_cmd_no_lock(handle, buffer, lba, lba_count, cb_func, cb_arg);
    comm_channel_controller_unlock_channel(channel_ctrlr, handle);
    return ret;
}

int comm_channel_send_write_cmd_no_lock(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    cmd_cb_func cb_func, void *cb_arg)
{
    channel_cmd_cb_ctx *cmd_cb_ctx = new_channel_cmd_cb_ctx(cb_func, cb_arg);
    if (cmd_cb_ctx == NULL)
        return ENOMEM;

    comm_channel_controller *g_channel_ctrlr = comm_channel_controller_get_instance();
    comm_channel_env *g_env = comm_channel_env_get_instance();
    int ret = spdk_nvme_ns_cmd_write(g_env->ns, g_channel_ctrlr->channels[handle].qpair, buffer, lba, lba_count, 
        channel_inner_spdk_cmd_callback, cmd_cb_ctx, 0);
    if (ret != 0)
    {
        ret = -ret;
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "spdk send write cmd failed.");
        free(cmd_cb_ctx);
        return ret;
    }
    return 0;
}

int comm_channel_send_write_cmd(comm_channel_handle handle, void *buffer, uint64_t lba, uint32_t lba_count,
    cmd_cb_func cb_func, void *cb_arg)
{
    int ret = 0;
    comm_channel_controller *channel_ctrlr = comm_channel_controller_get_instance();
    ret = comm_channel_controller_lock_channel(channel_ctrlr, handle);
    if (ret != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "send write cmd: lock channel failed.");
        return ret;
    }
    ret = comm_channel_send_write_cmd_no_lock(handle, buffer, lba, lba_count, cb_func, cb_arg);
    comm_channel_controller_unlock_channel(channel_ctrlr, handle);
    return ret;
}


static void build_nvme_cmd(struct spdk_nvme_cmd *nvme_cmd, comm_raw_cmd *raw_cmd)
{
    nvme_cmd->opc = raw_cmd->opcode;
    nvme_cmd->nsid = spdk_nvme_ns_get_id(comm_channel_env_get_instance()->ns);
    nvme_cmd->cdw10 = raw_cmd->dword10;
    nvme_cmd->cdw12 = raw_cmd->dword12;
    nvme_cmd->cdw13 = raw_cmd->dword13;
    nvme_cmd->cdw14 = raw_cmd->dword14;
    nvme_cmd->cdw15 = raw_cmd->dword15;
}

int comm_channel_send_raw_cmd_no_lock(comm_channel_handle handle, void *buf, uint32_t buf_len, 
    comm_raw_cmd *raw_cmd, cmd_cb_func cb_func, void *cb_arg)
{
    int ret = 0;
    channel_cmd_cb_ctx *cmd_cb_ctx = new_channel_cmd_cb_ctx(cb_func, cb_arg);
    if (cmd_cb_ctx == NULL)
        return ENOMEM;

    struct spdk_nvme_cmd *nvme_cmd = malloc(sizeof(struct spdk_nvme_cmd));
    if (nvme_cmd == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc nvme cmd failed.");
        ret = ENOMEM;
        goto err1;
    }
    build_nvme_cmd(nvme_cmd, raw_cmd);
    
    comm_channel_controller *g_channel_ctrlr = comm_channel_controller_get_instance();
    comm_channel_env *g_env = comm_channel_env_get_instance();
    ret = spdk_nvme_ctrlr_cmd_admin_raw(g_channel_ctrlr, nvme_cmd, buf, buf_len, 
        channel_inner_spdk_cmd_callback, cmd_cb_ctx);
    if (ret != 0)
    {
        ret = -ret;
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "spdk send raw cmd failed.");
        goto err2;
    }
    return ret;

    err2:
    free(nvme_cmd);
    err1:
    free(cmd_cb_ctx);
    return ret;
}

int comm_channel_send_raw_cmd(comm_channel_handle handle, void *buf, uint32_t buf_len, 
    comm_raw_cmd *raw_cmd, cmd_cb_func cb_func, void *cb_arg)
{
    int ret = 0;
    comm_channel_controller *channel_ctrlr = comm_channel_controller_get_instance();
    ret = comm_channel_controller_lock_channel(channel_ctrlr, handle);
    if (ret != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "send raw cmd: lock channel failed.");
        return ret;
    }
    ret = comm_channel_send_raw_cmd_no_lock(handle, buf, buf_len, raw_cmd, cb_func, cb_arg);
    comm_channel_controller_unlock_channel(channel_ctrlr, handle);
    return ret;
}

int polling_channel_completions_no_lock(comm_channel_handle handle, uint32_t max_cplt)
{
    comm_channel_controller *channel_ctrlr = comm_channel_controller_get_instance();
    int ret = spdk_nvme_qpair_process_completions(channel_ctrlr->channels[handle].qpair, max_cplt);
    return ret;
}