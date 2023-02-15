#include "communication/session_env.h"
#include "communication/session_cmd_ctx.h"
#include "utils/sys_log.h"

typedef TAILQ_HEAD(, comm_session_cmd_ctx) cmd_queue_t;

// 会话层环境
typedef struct comm_session_env
{
    cmd_queue_t cmd_queue;  // admin和I/O命令队列
    size_t cmd_queue_size;  // cmd_queue的大小
    pthread_cond_t polling_thread_cond;  // 轮询线程空闲且队列为空时进行等待的条件变量
    pthread_mutex_t cmd_queue_mtx;  // 保护命令队列，和polling_thread_cond相关的互斥锁
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
    ret = pthread_cond_init(&session_env->polling_thread_cond, NULL);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "init session env condvar failed.");
        return ret;
    }
    ret = pthread_mutex_init(&session_env->cmd_queue_mtx, NULL);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "init session env condmtx failed.");
        pthread_cond_destroy(&session_env->polling_thread_cond);
        return ret;
    }

    TAILQ_INIT(&session_env->cmd_queue);
    session_env->cmd_queue_size = 0;
    return ret;
}

int comm_session_env_submit_cmd_ctx(comm_session_cmd_ctx *cmd_ctx)
{
    // 是否需要volatile?? mutex能否保证session_env对象成员写入后其他线程的可见性？
    comm_session_env *session_env = session_env_get_instance();
    int ret = 0;
    ret = pthread_mutex_lock(&session_env->cmd_queue_mtx);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "lock session env failed.");
        return ret;
    }

    // 将cmd_ctx插入命令队列
    TAILQ_INSERT_TAIL(&session_env->cmd_queue, cmd_ctx, COMM_SESSION_CMD_QUEUE_FIELD);
    int need_wakeup_polling = session_env->cmd_queue_size == 0 ? 1 : 0;
    session_env->cmd_queue_size++;
    ret = pthread_mutex_unlock(&session_env->cmd_queue_mtx);
    if (ret != 0)
        HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, ret, "unlock session env failed.");
    
    // 若之前命令队列为空，则唤醒轮询线程
    if (need_wakeup_polling)
    {
        ret = pthread_cond_broadcast(&session_env->polling_thread_cond);
        if (ret != 0)
            HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, ret, "wakeup polling thread failed.");
    }

    return ret;
}

// 等待命令队列中有命令可获取，函数内会获取session_env锁，由轮询线程释放
static int polling_thread_wait_cmd_ready(comm_session_env *session_env)
{
    int ret = 0;
    ret = pthread_mutex_lock(&session_env->cmd_queue_mtx);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "polling thread lock session env failed.");
        return ret;
    }

    while (session_env->cmd_queue_size == 0)
    {
        ret = pthread_cond_wait(&session_env->polling_thread_cond, &session_env->cmd_queue_mtx);
        if (ret != 0)
        {
            HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "polling thread wait on condvar failed.");
            pthread_mutex_unlock(&session_env->cmd_queue_mtx);  // 轮询线程已经崩了，unlock没必要再判返回值
            return ret;
        }
    }

    return ret;
}

#define CPLT_TID_LIST_ENTRY list_entry
typedef struct cplt_tid_entry
{
    uint16_t tid;
    LIST_ENTRY(cplt_tid_entry) CPLT_TID_LIST_ENTRY;
} cplt_tid_entry;

typedef LIST_HEAD(, cplt_tid_entry) cplt_tid_list_t;

// 会话层轮询线程的执行环境
typedef struct polling_thread_env
{
    cmd_queue_t cq_queue, tid_queue;  // 命令CQ等待队列，tid等待队列
    cplt_tid_list_t cplt_tid_list;  // 从SSD查询得到的所有已完成tid
    cmd_queue_t err_queue;  // 若查询过程出错，将comm_session_cmd_ctx移入此队列

    /* epoll相关结构 */
    /* some code... */
} polling_thread_env;

static void polling_thread_move_cmd_to_tar_queue(cmd_queue_t *tar, cmd_queue_t *src, 
    comm_session_cmd_ctx *element)
{
    TAILQ_REMOVE(src, element, COMM_SESSION_CMD_QUEUE_FIELD);
    TAILQ_INSERT_TAIL(tar, element, COMM_SESSION_CMD_QUEUE_FIELD);
}

// 轮询线程处理一个已完成的命令
static void polling_thread_process_cplt_cmd(polling_thread_env *thrd_env, comm_session_cmd_ctx *cmd)
{
    /* 释放定时器 */
    /* some code... */

    // 如果是异步命令，则调用上层回调函数，然后释放其它资源
    if (cmd->cmd_sync_type == SESSION_ASYNC_CMD)
    {
        cmd->async_cb_func(cmd->cmd_result, cmd->async_cb_arg);
        comm_channel_release(cmd->channel);
        free_comm_session_cmd_ctx(cmd);
    }
    // 否则是同步命令，唤醒等待的线程
    else
    {
        int ret = pthread_mutex_lock(&cmd->wait_mtx);
        if (ret != 0)
        {
            HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "polling thread lock cond failed.");
            TAILQ_INSERT_TAIL(&thrd_env->err_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);
        }
        cmd->cmd_is_cplt = 1;  // 设置完成标志，同步命令根据此标志判断命令在会话层是否处理完成
        ret = pthread_mutex_unlock(&cmd->wait_mtx);
        if (ret != 0)
            HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, ret, "polling thread unlock cond failed.");
        
        // 通知通信层等待的线程
        ret = pthread_cond_broadcast(&cmd->wait_cond);
        if (ret != 0)
            HSCFS_LOG_ERRNO(HSCFS_LOG_WARNING, ret, "polling thread wakeup cond failed.");
    }
}

// 轮询线程处理一个在cq_queue上收到CQE的命令
static void polling_thread_process_cmd_received_CQE(polling_thread_env *thrd_env, comm_session_cmd_ctx *cmd)
{   
    TAILQ_REMOVE(&thrd_env->cq_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);

    // 如果命令不是长命令，则不需要再向SSD查询结果，命令已经完成
    if (!cmd->is_long_cmd)
        polling_thread_process_cplt_cmd(thrd_env, cmd);

    // 如果是长命令
    else
    {
        // 命令下发返回的CQE出错，不用再查询结果了
        if (cmd->cmd_result == COMM_CMD_CQE_ERROR)
            polling_thread_process_cplt_cmd(thrd_env, cmd);

        // 如果是查询命令结果的CQE，则直接将其重新移入tid等待列表
        // 否则，是命令下发的CQE，将状态置为SESSION_CMD_TID_WAIT_QUERY后加入tid等待列表
        if (cmd->cmd_session_state != SESSION_CMD_TID_CPLT_QUERY)
            cmd->cmd_session_state = SESSION_CMD_TID_WAIT_QUERY;

        TAILQ_INSERT_TAIL(&thrd_env->tid_queue, cmd, COMM_SESSION_CMD_QUEUE_FIELD);
    }
}

// 轮询线程处理cq队列的入口
static void polling_thread_process_cq_queue(polling_thread_env *thrd_env)
{
    TAILQ_HEAD(, comm_session_cmd_ctx) *cq_queue = &thrd_env->cq_queue;
    comm_session_cmd_ctx *cur_cmd, *nxt_cmd;
    TAILQ_FOREACH_SAFE(cur_cmd, cq_queue, COMM_SESSION_CMD_QUEUE_FIELD, nxt_cmd)
    {
        switch (cur_cmd->cmd_session_state)
        {
            // 当前命令为新加入cmd_queue，还未处理的命令
        case SESSION_CMD_NEW:
            /* 为其分配一个轮询定时器timer_fd，并将定时器fd加入epoll等待 */
            /* some code... */
            cur_cmd->cmd_session_state = SESSION_CMD_WAIT_POLLING;
            break;

            // 当前命令轮询定时器未到期，暂不轮询
        case SESSION_CMD_WAIT_POLLING:
            break;

            // 当前命令轮询定时器已到期，进行一次轮询
            // 定时器到期，状态置为NEED_POLLING，并disable定时器
            // 在channel上所有收到CQE的命令，轮询的回调函数将进行：
            // 1. 若状态为SESSION_CMD_WAIT_POLLING，则disable定时器
            // 2. 状态置为SESSION_CMD_RECEIVED_CQE或SESSION_CMD_TID_CPLT_QUERY
        case SESSION_CMD_NEED_POLLING:
            if (cur_cmd->cmd_nvme_type == SESSION_ADMIN_CMD)  // 命令是admin命令
            {
                int ret = comm_polling_admin_completions(cur_cmd->channel);
                if (ret < 0)  // polling出错，将该命令加入err_queue队列，不再轮询
                    polling_thread_move_cmd_to_tar_queue(&thrd_env->err_queue, cq_queue, cur_cmd);
            }
            else  // 命令是I/O命令
            {
                // 首先尝试对channel加锁
                int ret = comm_channel_trylock(cur_cmd->channel);
                if (ret != 0)
                {
                    // 若不是由于无法锁定，而是其它错误，将其加入err_queue队列
                    if (ret != EBUSY)
                        polling_thread_move_cmd_to_tar_queue(&thrd_env->err_queue, cq_queue, cur_cmd);
                    // 若无法锁定，则放弃这次轮询，等待下次
                }
                else  // 成功锁定channel，轮询该channel，对所有收到CQE的cmd都做处理
                {
                    int ret = comm_channel_polling_completions_no_lock(cur_cmd->channel, 0);
                    if (ret < 0)
                        polling_thread_move_cmd_to_tar_queue(&thrd_env->err_queue, cq_queue, cur_cmd);
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
    
    if (cmd->cmd_session_state == SESSION_CMD_WAIT_POLLING)
    {
        /* disable 定时器 */
    }

    cmd->cmd_session_state = SESSION_CMD_RECEIVED_CQE;

}

// 轮询线程发送tid结果查询命令的回调
// 设置命令结果状态和会话层状态
static void polling_thread_send_query_result_callback(comm_cmd_CQE_result result, void *arg)
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
    query_cmd.dword10 = cmd->tid_result_buf_len;
    query_cmd.dword12 = 0x60021;
    query_cmd.dword13 = cmd->tid;
    int ret = comm_send_raw_cmd(cmd->channel, cmd->tid_result_buffer, cmd->tid_result_buf_len, 
        &query_cmd, polling_thread_send_query_result_callback, cmd);
    if (ret != 0)
    {
        polling_thread_move_cmd_to_tar_queue(&thrd_env->err_queue, &thrd_env->tid_queue, cmd);
        return;
    }

    // 设置状态为等待进行CQE轮询
    cmd->cmd_session_state = SESSION_CMD_WAIT_POLLING;

    // 将该命令移入cq等待队列
    polling_thread_move_cmd_to_tar_queue(&thrd_env->cq_queue, &thrd_env->tid_queue, cmd);
    
    /* 启动该命令的定时器并加入epoll等待 */
    /* some code... */
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
            if (cmd->tid == tid)
                cmd->cmd_session_state = SESSION_CMD_TID_CAN_QUERY;
            LIST_REMOVE(cur_tid_entry, CPLT_TID_LIST_ENTRY);
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
            polling_thread_process_cplt_cmd(thrd_env, cmd);
            break;

            // 忽略SESSION_CMD_TID_WAIT_QUERY，等待状态变为SESSION_CMD_TID_CAN_QUERY再处理
        default:
            break;
        }
    }
}

// 会话层轮询线程
void* comm_session_polling_thread(void *arg)
{
    comm_session_env *session_env = session_env_get_instance();
    polling_thread_env thrd_env;
    TAILQ_INIT(&thrd_env.cq_queue);
    TAILQ_INIT(&thrd_env.tid_queue);
    LIST_INIT(&thrd_env.cplt_tid_list);

    while (1)
    {
        // 等待有命令可以取
        int ret = polling_thread_wait_cmd_ready(session_env);
        if (ret != 0)
        {
            HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "polling thread exit.");
            return NULL;
        }

        // 将命令队列中的内容转移到线程内部，并把命令队列清空
        TAILQ_CONCAT(&thrd_env.cq_queue, &session_env->cmd_queue, COMM_SESSION_CMD_QUEUE_FIELD);
        session_env->cmd_queue_size = 0;
        ret = pthread_mutex_unlock(&session_env->cmd_queue_mtx);

        // 轮询CQE等待队列
        polling_thread_process_cq_queue(&thrd_env);

        // 轮询tid等待队列
        polling_thread_process_tid_queue(&thrd_env);

        /* epoll_wait，等待：CQE查询的定时器到期，或已完成tid查询的定时器到期 */
        /* 将到期的定时器移出epoll */
        /* 根据到期的定时器设置对应命令的状态 */
        
        /* 若tid等待队列非空，且已完成tid查询的定时器到期 */
        /* 向SSD查询已完成tid，并将它们合并入cplt_tid_list */

        /* 若thrd_env中cq_queue和tid_queue均为空，则睡眠等待上层命令，否则继续loop */
    }
}