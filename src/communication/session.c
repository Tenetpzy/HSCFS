#include <errno.h>
#include <stdlib.h>
#include <stdatomic.h>

#include "communication/session.h"
#include "communication/memory.h"
#include "utils/hscfs_log.h"
#include "utils/hscfs_timer.h"

/*******************************************************************************************/
/* 会话层上下文分配、初始化、释放 */

// 初始化同步命令的命令类型(同步/异步标志)、条件变量相关信息
static int comm_session_cmd_ctx_init_sync_info(comm_session_cmd_ctx *ctx)
{
    int ret = 0;
    ret = cond_init(&ctx->wait_cond);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "init session cmd ctx condvar failed.");
        return ret;
    }
    ret = mutex_init(&ctx->wait_mtx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "init session cmd ctx condmtx failed.");
        pthread_cond_destroy(&ctx->wait_cond);  // 不会返回error
    }
    ctx->cmd_sync_type = SESSION_SYNC_CMD;
    ctx->cmd_is_cplt = 0;
    return ret;
}

// 初始化异步命令的命令类型(同步/异步标志)，回调相关信息
static void comm_session_cmd_ctx_init_async_info(comm_session_cmd_ctx *ctx, 
    comm_async_cb_func cb_func, void *cb_arg, uint8_t take_ownership)
{
    ctx->cmd_sync_type = SESSION_ASYNC_CMD;
    ctx->async_cb_func = cb_func;
    ctx->async_cb_arg = cb_arg;
    ctx->has_ownership = take_ownership;
}

// 分配新的tid。同时下发多于UINT16_MAX个长命令时会出现tid重复，可能导致错误，暂时不处理。
static uint16_t alloc_new_tid(void)
{
    static _Atomic uint16_t tid = 0;
    if (tid == UINT16_MAX)
        tid = 0;
    uint16_t ret = tid + 1;
    atomic_fetch_add(&tid, 1);
    return ret;
}

// 分配comm_session_cmd_ctx结构体，并初始化属性
static int comm_session_cmd_ctx_contrustor_inner(comm_session_cmd_ctx *self, comm_channel_handle channel,
    comm_session_cmd_nvme_type cmd_nvme_type, comm_session_cmd_sync_attr sync_type, 
    comm_async_cb_func cb_func, void *cb_arg, uint8_t take_ownership, 
    uint8_t long_cmd, void *res_buf, uint32_t res_len)
{
    if (sync_type == SESSION_SYNC_CMD)
    {
        int ret = comm_session_cmd_ctx_init_sync_info(self);
        if (ret != 0)
            return ret;
    }
    else
        comm_session_cmd_ctx_init_async_info(self, cb_func, cb_arg, take_ownership);

    if (long_cmd)
    {
        self->tid = alloc_new_tid();
        self->tid_result_buffer = res_buf;
        self->tid_result_buf_len = res_len;
    }
    self->is_long_cmd = long_cmd;
    
    self->channel = channel;
    self->cmd_nvme_type = cmd_nvme_type;
    self->cmd_session_state = SESSION_CMD_NEED_POLLING;

    return 0;
}

int comm_session_sync_cmd_ctx_constructor(comm_session_cmd_ctx *self, comm_channel_handle channel,
     comm_session_cmd_nvme_type cmd_type)
{
    return comm_session_cmd_ctx_contrustor_inner(self, channel, cmd_type, SESSION_SYNC_CMD, 
        NULL, NULL, 0, 0, NULL, 0);
}

int comm_session_async_cmd_ctx_constructor(comm_session_cmd_ctx *self, comm_channel_handle channel, 
    comm_session_cmd_nvme_type cmd_type, comm_async_cb_func cb_func, void *cb_arg, uint8_t take_ownership)
{
    return comm_session_cmd_ctx_contrustor_inner(self, channel, cmd_type, SESSION_ASYNC_CMD, 
        cb_func, cb_arg, take_ownership, 0, NULL, 0);
}

int comm_session_sync_long_cmd_ctx_constructor(comm_session_cmd_ctx *self, 
    comm_channel_handle channel, void *tid_res_buf, uint32_t tid_res_len)
{
    return comm_session_cmd_ctx_contrustor_inner(self, channel, SESSION_ADMIN_CMD, SESSION_SYNC_CMD,
        NULL, NULL, 0, 1, tid_res_buf, tid_res_len);
}

int comm_session_async_long_cmd_ctx_constructor(comm_session_cmd_ctx *self, comm_channel_handle channel,
    void *tid_res_buf, uint32_t tid_res_len, comm_async_cb_func cb_func, void *cb_arg, uint8_t take_ownership)
{
    return comm_session_cmd_ctx_contrustor_inner(self, channel, SESSION_ADMIN_CMD, SESSION_ASYNC_CMD,
        cb_func, cb_arg, take_ownership, 1, tid_res_buf, tid_res_len);
}

// 释放comm_session_cmd_ctx的资源(对于同步命令，则释放条件变量和锁；异步命令无资源需释放)
void comm_session_cmd_ctx_destructor(comm_session_cmd_ctx *self)
{
    if (self->cmd_sync_type == SESSION_SYNC_CMD)
    {
        int ret = mutex_destroy(&self->wait_mtx);
        if (ret != 0)
            HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "destroy session cmd ctx wait_mtx failed.");
        ret = pthread_cond_destroy(&self->wait_cond);
        if (ret != 0)
            HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "destroy session cmd ctx wait_cond failed.");
    }
}

/*****************************************************************************************/
/* 会话层环境 */

typedef TAILQ_HEAD(, comm_session_cmd_ctx) cmd_queue_t;

typedef struct comm_session_env
{
    cmd_queue_t cmd_queue;  // admin和I/O命令队列
    size_t cmd_queue_size;  // cmd_queue的大小
    cond_t polling_thread_cond;  // 轮询线程空闲且队列为空时进行等待的条件变量
    mutex_t cmd_queue_mtx;  // 保护命令队列，和polling_thread_cond相关的互斥锁
} comm_session_env;

static comm_session_env* session_env_get_instance(void)
{
    static comm_session_env session_env;
    return &session_env;
}

int comm_session_env_init(void)
{
    comm_session_env *session_env = session_env_get_instance();
    int ret = 0;
    ret = cond_init(&session_env->polling_thread_cond);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "init session env condvar failed.");
        return ret;
    }
    ret = mutex_init(&session_env->cmd_queue_mtx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "init session env condmtx failed.");
        pthread_cond_destroy(&session_env->polling_thread_cond);
        return ret;
    }

    TAILQ_INIT(&session_env->cmd_queue);
    session_env->cmd_queue_size = 0;
    return ret;
}

int comm_session_submit_cmd_ctx(comm_session_cmd_ctx *cmd_ctx)
{
    // 是否需要volatile?? mutex能否保证session_env对象成员写入后其他线程的可见性？
    comm_session_env *session_env = session_env_get_instance();
    int ret = 0;
    ret = mutex_lock(&session_env->cmd_queue_mtx);
    if (ret != 0)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "lock session env failed.");
        return ret;
    }

    // 将cmd_ctx插入命令队列
    TAILQ_INSERT_TAIL(&session_env->cmd_queue, cmd_ctx, COMM_SESSION_CMD_QUEUE_FIELD);
    // int need_wakeup_polling = session_env->cmd_queue_size == 0 ? 1 : 0;
    session_env->cmd_queue_size++;
    ret = mutex_unlock(&session_env->cmd_queue_mtx);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "unlock session env failed.");
    
    // 若之前命令队列为空，则唤醒轮询线程
    // if (need_wakeup_polling)
    // {
        ret = cond_broadcast(&session_env->polling_thread_cond);
        if (ret != 0)
            HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "wakeup polling thread failed.");
    // }

    return ret;
}


/***********************************************************/
/* 轮询线程 */


#define CPLT_TID_LIST_ENTRY list_entry
typedef struct cplt_tid_entry
{
    uint16_t tid;
    LIST_ENTRY(cplt_tid_entry) CPLT_TID_LIST_ENTRY;
} cplt_tid_entry;

typedef LIST_HEAD(, cplt_tid_entry) cplt_tid_list_t;

// 轮询线程中，[获取已完成tid]任务的状态
typedef enum cplt_tid_poll_state 
{
    CPLT_TID_POLL_DISABLE, // 此时未进行任务
    CPLT_TID_POLL_WAIT_PERIOD,  // 等待到达任务轮询周期 
    CPLT_TID_POLL_RUNNING, // 任务正在进行
    CPLT_TID_POLL_FINISHED,  // 已成功完成该任务
    CPLT_TID_POLL_ERROR  // 该任务执行过程出错
} cplt_tid_polling_state;

// 主机每次进行[获取已完成tid]任务时，指定的tid列表大小
// 即每次最多从SSD获取CPLT_TID_PER_POLL个已完成tid
#define CPLT_TID_PER_POLL 8

// 发送[获取已完成tid]命令的时间周期(ns)
// 50us
#define CPLT_TID_POLL_PERIOD (1000 * 50)

#define INVALID_TID 0

// 会话层轮询线程的执行环境
typedef struct polling_thread_env
{
    cmd_queue_t cq_queue, tid_queue;  // 命令CQ等待队列，tid等待队列
    cplt_tid_list_t cplt_tid_list;  // 从SSD查询得到的所有已完成tid
    cmd_queue_t err_queue;  // 若查询过程出错，将comm_session_cmd_ctx移入此队列

    hscfs_timer cplt_tid_query_timer;  // 查询已完成tid的任务定时器
    uint16_t *cplt_tid_buffer;  // 用于保存已完成tid列表的buffer
    cplt_tid_polling_state cplt_tid_query_state;  // 查询已完成tid任务的状态
    comm_session_cmd_ctx cplt_tid_query_ctx;  // 查询已完成tid任务的上下文
    comm_channel_handle cplt_tid_query_handle;  // 查询已完成tid任务使用的channel
    comm_dev *dev;  // 轮询的目标设备
} polling_thread_env;

// 轮询线程向SSD发送[获取已完成tid]所使用的会话层回调。回调参数为polling_thread_env
static void polling_thread_cplt_tid_query_callback(comm_cmd_result res, void *arg)
{
    polling_thread_env *thrd_env = (polling_thread_env *)arg;
    if (res != COMM_CMD_SUCCESS)
    {
        HSCFS_LOG(HSCFS_LOG_WARNING, "polling thread query cplt tid failed.");
        thrd_env->cplt_tid_query_state = CPLT_TID_POLL_ERROR;
        return;
    }

    // 标记[获取已完成tid]任务已经成功获得结果
    thrd_env->cplt_tid_query_state = CPLT_TID_POLL_FINISHED;
}

static int polling_thread_env_constructor(polling_thread_env *self, comm_dev *device)
{
    TAILQ_INIT(&self->cq_queue);
    TAILQ_INIT(&self->tid_queue);
    TAILQ_INIT(&self->err_queue);
    LIST_INIT(&self->cplt_tid_list);

    int ret = hscfs_timer_constructor(&self->cplt_tid_query_timer, 0);
    if (ret != 0)
        return ret;
    struct timespec tim = {.tv_sec = 0, .tv_nsec = CPLT_TID_POLL_PERIOD};
    hscfs_timer_set(&self->cplt_tid_query_timer, &tim, 0);

    self->cplt_tid_buffer = comm_alloc_dma_mem(sizeof(uint16_t) * CPLT_TID_PER_POLL);
    if (self->cplt_tid_buffer == NULL)
    {
        hscfs_timer_destructor(&self->cplt_tid_query_timer);
        return ENOMEM;
    }

    self->cplt_tid_query_handle = comm_channel_controller_get_channel(&device->channel_ctrlr);
    self->cplt_tid_query_state = CPLT_TID_POLL_DISABLE;
    self->dev = device;
    // cplt_tid_query_ctx在每次发送该命令时初始化

    return 0;
}

static void polling_thread_env_destructor(polling_thread_env *self)
{
    hscfs_timer_destructor(&self->cplt_tid_query_timer);
    comm_free_dma_mem(self->cplt_tid_buffer);
}

static void polling_thread_move_cmd_between_queues(cmd_queue_t *tar, cmd_queue_t *src, 
    comm_session_cmd_ctx *element)
{
    TAILQ_REMOVE(src, element, COMM_SESSION_CMD_QUEUE_FIELD);
    TAILQ_INSERT_TAIL(tar, element, COMM_SESSION_CMD_QUEUE_FIELD);
}

// 轮询线程处理一个已完成的命令。返回EBUSY则暂时无法获取该命令的锁，应当下次遍历时重试
static int polling_thread_process_cplt_cmd(polling_thread_env *thrd_env, comm_session_cmd_ctx *cmd)
{
    // 如果是异步命令，则调用上层回调函数，若有所有权，则释放资源
    if (cmd->cmd_sync_type == SESSION_ASYNC_CMD)
    {
        cmd->async_cb_func(cmd->cmd_result, cmd->async_cb_arg);
        if (cmd->has_ownership)
        {
            comm_channel_release(cmd->channel);
            free(cmd);
        }
    }

    // 否则是同步命令，唤醒等待的线程
    else
    {
        int ret = mutex_trylock(&cmd->wait_mtx);
        if (ret != 0)
        {
            if (ret == EBUSY)
                return EBUSY;
            HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "polling thread lock cond failed.");
            TAILQ_INSERT_TAIL(&thrd_env->err_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);
            return 0;
        }
        cmd->cmd_is_cplt = 1;  // 设置完成标志，同步命令根据此标志判断命令在会话层是否处理完成
        ret = mutex_unlock(&cmd->wait_mtx);
        if (ret != 0)
            HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "polling thread unlock cond failed.");
        
        // 通知通信层等待的线程
        ret = cond_broadcast(&cmd->wait_cond);
        if (ret != 0)
            HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "polling thread wakeup cond failed.");
    }

    return 0;
}

// 轮询线程处理一个在cq_queue上收到CQE的命令
static void polling_thread_process_cmd_received_CQE(polling_thread_env *thrd_env, comm_session_cmd_ctx *cmd)
{   
    TAILQ_REMOVE(&thrd_env->cq_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);

    // 如果命令不是长命令，则不需要再向SSD查询结果，命令已经完成
    if (!cmd->is_long_cmd)
    {
        if (polling_thread_process_cplt_cmd(thrd_env, cmd) == EBUSY)
            TAILQ_INSERT_TAIL(&thrd_env->cq_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);
    }

    // 如果是长命令
    else
    {
        // 命令下发返回的CQE出错，不用再查询结果了
        if (cmd->cmd_result == COMM_CMD_CQE_ERROR)
        {
            if (polling_thread_process_cplt_cmd(thrd_env, cmd) == EBUSY)
                TAILQ_INSERT_TAIL(&thrd_env->err_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);
        }

        // 如果是命令下发的CQE，将状态置为SESSION_CMD_TID_WAIT_QUERY后加入tid等待列表
        if (cmd->cmd_session_state == SESSION_CMD_RECEIVED_CQE)
            cmd->cmd_session_state = SESSION_CMD_TID_WAIT_QUERY;

        // 否则是查询长命令结果的CQE(cmd_session_state == SESSION_CMD_TID_CPLT_QUERY)
        // 则直接将其重新移入tid等待列表
        TAILQ_INSERT_TAIL(&thrd_env->tid_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);
    }
}

// 轮询线程处理cq队列的入口
static void polling_thread_process_cq_queue(polling_thread_env *thrd_env)
{
    cmd_queue_t *cq_queue = &thrd_env->cq_queue;
    comm_session_cmd_ctx *cur_cmd, *nxt_cmd;
    TAILQ_FOREACH_SAFE(cur_cmd, cq_queue, COMM_SESSION_CMD_QUEUE_FIELD, nxt_cmd)
    {
        switch (cur_cmd->cmd_session_state)
        {
            // 在channel上所有收到CQE的命令，轮询的回调函数将
            // 把状态置为SESSION_CMD_RECEIVED_CQE或SESSION_CMD_TID_CPLT_QUERY
        case SESSION_CMD_NEED_POLLING:
            if (cur_cmd->cmd_nvme_type == SESSION_ADMIN_CMD)  // 命令是admin命令
            {
                int ret = comm_polling_admin_completions(cur_cmd->channel);
                if (ret < 0)  // polling出错，将该命令加入err_queue队列，不再轮询
                    polling_thread_move_cmd_between_queues(&thrd_env->err_queue, cq_queue, cur_cmd);
            }
            else  // 命令是I/O命令
            {
                // 首先尝试对channel加锁
                int ret = comm_channel_trylock(cur_cmd->channel);
                if (ret != 0)
                {
                    // 若不是由于无法锁定，而是其它错误，将其加入err_queue队列
                    if (ret != EBUSY)
                        polling_thread_move_cmd_between_queues(&thrd_env->err_queue, cq_queue, cur_cmd);
                    // 若无法锁定，则放弃这次轮询，等待下次
                }
                else  // 成功锁定channel，轮询该channel，对所有收到CQE的cmd都做处理
                {
                    int ret = comm_channel_polling_completions_no_lock(cur_cmd->channel, 0);
                    if (ret < 0)
                        polling_thread_move_cmd_between_queues(&thrd_env->err_queue, cq_queue, cur_cmd);
                    comm_channel_unlock(cur_cmd->channel);
                }
            }

            // 轮询后确定已经收到CQE，进行后续处理
            if (cur_cmd->cmd_session_state == SESSION_CMD_RECEIVED_CQE ||
                cur_cmd->cmd_session_state == SESSION_CMD_TID_CPLT_QUERY)
                polling_thread_process_cmd_received_CQE(thrd_env, cur_cmd);
            break;

            // 若当前命令是因为在处理其它命令时，进行了轮询操作，确认了当前命令也收到CQE
            // 直接进行后续处理
        case SESSION_CMD_RECEIVED_CQE:
        case SESSION_CMD_TID_CPLT_QUERY:
            polling_thread_process_cmd_received_CQE(thrd_env, cur_cmd);
            break;

        default:
            break;
        }
    }
}

// 轮询线程提供给通信层的CQE回调
void comm_session_polling_thread_callback(comm_cmd_CQE_result result, void *arg)
{
    comm_session_cmd_ctx *cmd = (comm_session_cmd_ctx *)arg;
    if (result == CMD_CQE_ERROR)
        cmd->cmd_result = COMM_CMD_CQE_ERROR;
    else
        cmd->cmd_result = COMM_CMD_SUCCESS;

    cmd->cmd_session_state = SESSION_CMD_RECEIVED_CQE;
}

// 轮询线程发送[长命令结果查询]命令的回调
static void polling_thread_query_result_callback(comm_cmd_CQE_result result, void *arg)
{
    comm_session_cmd_ctx *cmd = (comm_session_cmd_ctx *)arg;
    if (result == CMD_CQE_ERROR)
        cmd->cmd_result = COMM_CMD_TID_QUERY_ERROR;
    else
        cmd->cmd_result = COMM_CMD_SUCCESS;
    
    // 会话层状态置为已完成结果查询
    // 后续命令将在polling_thread_process_cq_queue中重新移入tid等待队列
    cmd->cmd_session_state = SESSION_CMD_TID_CPLT_QUERY;
}

// 发送查询长命令cmd执行结果的命令
static void polling_thread_send_query_result_cmd(polling_thread_env *thrd_env, comm_session_cmd_ctx *cmd)
{
    comm_raw_cmd query_cmd = {0};
    query_cmd.opcode = 0xc2;
    query_cmd.dword10 = cmd->tid_result_buf_len / 4;
    query_cmd.dword12 = 0x60021;
    query_cmd.dword13 = cmd->tid;
    int ret = comm_send_raw_cmd(cmd->channel, cmd->tid_result_buffer, cmd->tid_result_buf_len, 
        &query_cmd, polling_thread_query_result_callback, cmd);
    if (ret != 0)
    {
        polling_thread_move_cmd_between_queues(&thrd_env->err_queue, &thrd_env->tid_queue, cmd);
        return;
    }

    // 设置状态为等待进行CQE轮询并移入cq等待队列，等待cq轮询子任务轮询其结果查询命令的CQE
    cmd->cmd_session_state = SESSION_CMD_NEED_POLLING;
    polling_thread_move_cmd_between_queues(&thrd_env->cq_queue, &thrd_env->tid_queue, cmd);
}

// 轮询线程处理tid等待队列的入口
static void polling_thread_process_tid_queue(polling_thread_env *thrd_env)
{
    cplt_tid_list_t *cplt_tid_list = &thrd_env->cplt_tid_list;
    cmd_queue_t *tid_wait_queue = &thrd_env->tid_queue;
    cplt_tid_entry *cur_tid_entry, *nxt_tid_entry;
    comm_session_cmd_ctx *cmd, *nxt_cmd;

    // 对已完成tid链表中的每个tid，遍历tid等待队列，找到对应命令并置为状态
    // 并将该tid从已完成tid链表中移除
    LIST_FOREACH_SAFE(cur_tid_entry, cplt_tid_list, CPLT_TID_LIST_ENTRY, nxt_tid_entry)
    {
        uint16_t tid = cur_tid_entry->tid;
        TAILQ_FOREACH(cmd, tid_wait_queue, COMM_SESSION_CMD_QUEUE_FIELD)
        {
            if (cmd->tid == tid && cmd->cmd_session_state == SESSION_CMD_TID_WAIT_QUERY)
            {
                cmd->cmd_session_state = SESSION_CMD_TID_CAN_QUERY;
                LIST_REMOVE(cur_tid_entry, CPLT_TID_LIST_ENTRY);
                free(cur_tid_entry);
            }
        }
    }

    TAILQ_FOREACH_SAFE(cmd, tid_wait_queue, COMM_SESSION_CMD_QUEUE_FIELD, nxt_cmd)
    {
        switch (cmd->cmd_session_state)
        {
            // 当前长命令已经可以查询结果
        case SESSION_CMD_TID_CAN_QUERY:
            polling_thread_send_query_result_cmd(thrd_env, cmd);
            break;

            // 当前已经收到查询结果命令的CQE
        case SESSION_CMD_TID_CPLT_QUERY:
            TAILQ_REMOVE(tid_wait_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);
            if (polling_thread_process_cplt_cmd(thrd_env, cmd) == EBUSY)
                TAILQ_INSERT_TAIL(tid_wait_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);
            break;

            // 忽略SESSION_CMD_TID_WAIT_QUERY，等待状态变为SESSION_CMD_TID_CAN_QUERY再处理
        default:
            break;
        }
    }
}

// 轮询线程发送[查询已完成tid命令]
static int polling_thread_send_query_cplt_tid_cmd(polling_thread_env *thrd_env)
{
    // 下发命令
    comm_raw_cmd cmd = {0};
    cmd.opcode = 0xc2;
    cmd.dword10 = CPLT_TID_PER_POLL * sizeof(uint16_t) / 4;
    cmd.dword12 = 0x50021;
    cmd.dword13 = 0;
    comm_session_async_cmd_ctx_constructor(&thrd_env->cplt_tid_query_ctx, thrd_env->cplt_tid_query_handle, 
        SESSION_ADMIN_CMD, polling_thread_cplt_tid_query_callback, thrd_env, 0);
    int ret = comm_send_raw_cmd(thrd_env->cplt_tid_query_handle, thrd_env->cplt_tid_buffer, 
        CPLT_TID_PER_POLL * sizeof(uint16_t), &cmd, 
        comm_session_polling_thread_callback, &thrd_env->cplt_tid_query_ctx);
    if (ret != 0)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "polling thread send cplt tid query cmd failed.");
        return ret;
    }

    // 将命令加入cq队列
    TAILQ_INSERT_TAIL(&thrd_env->cq_queue, &thrd_env->cplt_tid_query_ctx, COMM_SESSION_CMD_QUEUE_FIELD);
    return 0;
}

// 轮询线程处理[已完成tid查询]的入口
static void polling_thread_process_cplt_tid_query(polling_thread_env *thrd_env)
{
    int ret = 0;
    switch (thrd_env->cplt_tid_query_state)
    {
        /* 此时已完成tid查询任务未激活
         * 如果tid等待队列不为空，则启动该任务（启动轮询定时器）
         */
    case CPLT_TID_POLL_DISABLE:
        if (!TAILQ_EMPTY(&thrd_env->tid_queue))
        {
            ret = hscfs_timer_start(&thrd_env->cplt_tid_query_timer);
            if (ret != 0)
            {
                HSCFS_LOG(HSCFS_LOG_ERROR, "polling thread start cplt tid query timer failed.");
                thrd_env->cplt_tid_query_state = CPLT_TID_POLL_ERROR;
                return;
            }
            thrd_env->cplt_tid_query_state = CPLT_TID_POLL_WAIT_PERIOD;
        }
        break;

        /* 此时正在等待轮询周期
         * 则轮询定时器是否到期，若到期，发送命令
         */
    case CPLT_TID_POLL_WAIT_PERIOD:
        ret = hscfs_timer_check_expire(&thrd_env->cplt_tid_query_timer, NULL);
        if (ret != 0) // 未到期或发生错误
        {
            if (ret == EAGAIN)
                return;
            HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "polling thread poll cplt tid query timer failed.");
            thrd_env->cplt_tid_query_state = CPLT_TID_POLL_ERROR;
            return;
        }

        // 定时器已到期，发送查询已完成tid命令，标记任务正在进行
        ret = polling_thread_send_query_cplt_tid_cmd(thrd_env);
        if (ret != 0)
        {
            thrd_env->cplt_tid_query_state = CPLT_TID_POLL_ERROR;
            return;
        }
        thrd_env->cplt_tid_query_state = CPLT_TID_POLL_RUNNING;
        break;

    // 发送命令后，由回调函数将状态设置为CPLT_TID_POLL_FINISHED，表示已经收到CQE
    case CPLT_TID_POLL_FINISHED:
        // 将所有收到的tid加入cplt tid链表
        for (size_t i = 0; i < CPLT_TID_PER_POLL; ++i)
        {
            if (thrd_env->cplt_tid_buffer[i] == INVALID_TID)
                break;
            cplt_tid_entry *entry = (cplt_tid_entry *)malloc(sizeof(cplt_tid_entry));
            if (entry == NULL)
            {
                HSCFS_LOG(HSCFS_LOG_WARNING, "polling thread alloc cplt tid entry failed.");
                return;
            }
            entry->tid = thrd_env->cplt_tid_buffer[i];
            LIST_INSERT_HEAD(&thrd_env->cplt_tid_list, entry, CPLT_TID_LIST_ENTRY);
        }
        thrd_env->cplt_tid_query_state = CPLT_TID_POLL_DISABLE;
        break;
    
    default:
        break;
    }
}

// 判断轮询线程是否活跃
static int polling_thread_is_working(polling_thread_env *thrd_env)
{
    // 如果cq等待队列或tid等待队列中还有任务要处理，则轮询线程是活跃的
    return !(TAILQ_EMPTY(&thrd_env->cq_queue) && TAILQ_EMPTY(&thrd_env->tid_queue));
}

/* 
 * 从会话层命令队列中取新命令。
 * 若轮询线程不活跃，且命令队列中没有新命令，则睡眠等待。
 * 若轮询线程活跃，则尝试从命令队列中获取新命令。
 * 返回0则发生panic，需要终止轮询线程。
 */ 
static int polling_thread_fetch_cmd_from_session(polling_thread_env *thrd_env, comm_session_env *session_env)
{
    int ret = 0;

    // 如果当前轮询线程活跃，则只尝试从会话层获取更多的命令。
    // 此处尝试对命令队列加锁。
    if (polling_thread_is_working(thrd_env))
    {
        ret = mutex_trylock(&session_env->cmd_queue_mtx);
        if (ret != 0)
        {
            // 命令队列被接口层占用，放弃本次获取命令
            if (ret == EBUSY)
                return 0;
            HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "polling thread trylock session env failed.");
            return 0;
        }
    }

    // 如果当前轮询线程没有在工作，则睡眠等待会话层有命令可取
    else
    {
        ret = mutex_lock(&session_env->cmd_queue_mtx);
        if (ret != 0)
        {
            HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "polling thread lock session env failed.");
            return ret;
        }
        while (session_env->cmd_queue_size == 0)
        {
            ret = cond_wait(&session_env->polling_thread_cond, &session_env->cmd_queue_mtx);
            if (ret != 0)
            {
                HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "polling thread wait on condvar failed.");
                mutex_unlock(&session_env->cmd_queue_mtx);  // 轮询线程已经崩了，unlock没必要再判返回值
                return ret;
            }
        }
    }

    // 将会话层命令队列中的内容转移到线程内部的cq等待队列，并把命令队列清空
    TAILQ_CONCAT(&thrd_env->cq_queue, &session_env->cmd_queue, COMM_SESSION_CMD_QUEUE_FIELD);
    session_env->cmd_queue_size = 0;
    ret = mutex_unlock(&session_env->cmd_queue_mtx);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "polling thread unlock session env failed.");

    return 0;
}

static int polling_thread_is_error(polling_thread_env *thrd_env)
{
    return !TAILQ_EMPTY(&thrd_env->err_queue) || thrd_env->cplt_tid_query_state == CPLT_TID_POLL_ERROR;
}

static void polling_thread_print_exit_log(void)
{
    HSCFS_LOG(HSCFS_LOG_ERROR, "polling thread exit.");
}

/* 
 * 会话层轮询线程
 * todo: 目前此线程没有设置取消机制。
 */
void* comm_session_polling_thread(void *arg)
{
    comm_session_env *session_env = session_env_get_instance();
    polling_thread_env thrd_env;
    if (polling_thread_env_constructor(&thrd_env, ((polling_thread_start_env*)arg)->dev) != 0)
    {
        polling_thread_print_exit_log();
        return NULL;
    }

    while (1)
    {
        // 等待有命令可以取
        int ret = polling_thread_fetch_cmd_from_session(&thrd_env, session_env);
        if (ret != 0)
        {
            HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "polling thread fetch cmd failed.");
            polling_thread_print_exit_log();
            return NULL;
        }

        polling_thread_process_cq_queue(&thrd_env);  // 轮询cq等待队列
        polling_thread_process_tid_queue(&thrd_env);  // 轮询tid等待队列
        polling_thread_process_cplt_tid_query(&thrd_env);  // 处理已完成tid轮询任务

        if (polling_thread_is_error(&thrd_env))
        {
            HSCFS_LOG(HSCFS_LOG_ERROR, "polling thread error occured.");
            polling_thread_print_exit_log();
            return NULL;
        }
    }
}
