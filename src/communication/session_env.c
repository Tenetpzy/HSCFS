#include "communication/session_env.h"
#include "communication/session_cmd_ctx.h"
#include "utils/sys_log.h"

// 会话层的命令队列结构，包含队列本身与队列大小
typedef struct comm_session_cmd_queue
{
    TAILQ_HEAD(, comm_session_cmd_ctx) cmd_queue;
    size_t queue_size;
} comm_session_cmd_queue;

// 会话层环境，包含I/O和admin等待队列
typedef struct comm_session_env
{
    // 等待轮询线程处理的I/O命令队列
    comm_session_cmd_queue io_cmd_queue;

    // 等待轮询线程处理的admin命令队列
    comm_session_cmd_queue admin_cmd_queue;

    size_t cmd_queue_total_size;  // 两个队列的总大小
    pthread_cond_t polling_thread_cond;  // 轮询线程空闲且队列为空时进行等待的条件变量
    pthread_mutex_t cmd_queue_mtx;  // 保护命令队列，和polling_thread_cond相关的互斥锁
} comm_session_env;

static void comm_session_cmd_queue_init(comm_session_cmd_queue *queue)
{
    TAILQ_INIT(&queue->cmd_queue);
    queue->queue_size = 0;
}

static void comm_session_cmd_queue_push_back(comm_session_cmd_queue *queue, 
    comm_session_cmd_ctx *element)
{
    TAILQ_INSERT_TAIL(&queue->cmd_queue, element, COMM_SESSION_CMD_QUEUE_FIELD);
    queue->queue_size++;
}

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

    comm_session_cmd_queue_init(&session_env->admin_cmd_queue);
    comm_session_cmd_queue_init(&session_env->io_cmd_queue);
    session_env->cmd_queue_total_size = 0;
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

    // 将cmd_ctx插入admin或I/O队列
    comm_session_cmd_nvme_type nvme_type = comm_session_cmd_ctx_get_nvme_type(cmd_ctx);
    if (nvme_type == SESSION_ADMIN_CMD)
        comm_session_cmd_queue_push_back(&session_env->admin_cmd_queue, cmd_ctx);
    else  // nvme_type == SESSION_IO_CMD
        comm_session_cmd_queue_push_back(&session_env->io_cmd_queue, cmd_ctx);

    int need_wakeup_polling = session_env->cmd_queue_total_size == 0 ? 1 : 0;
    session_env->cmd_queue_total_size++;
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

    while (session_env->cmd_queue_total_size == 0)
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

// 将src_queue中的所有元素转移到tar_queue末尾，然后置src_queue为空
static void polling_thread_fetch_cmd(comm_session_cmd_queue *tar_queue, comm_session_cmd_queue *src_queue)
{
    TAILQ_CONCAT(&tar_queue->cmd_queue, &src_queue->cmd_queue, COMM_SESSION_CMD_QUEUE_FIELD);
    tar_queue->queue_size = src_queue->queue_size;
    src_queue->queue_size = 0;
}

// 会话层轮询线程的执行环境
typedef struct polling_thread_env
{
    comm_session_cmd_queue admin_queue, io_queue, tid_queue;
} polling_thread_env;

// 会话层轮询线程
void* comm_session_polling_thread(void *arg)
{
    comm_session_env *session_env = session_env_get_instance();
    comm_session_cmd_queue admin_queue, io_queue;
    comm_session_cmd_queue_init(&admin_queue);
    comm_session_cmd_queue_init(&io_queue);

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
        if (session_env->admin_cmd_queue.queue_size != 0)
            polling_thread_fetch_cmd(&admin_queue, &session_env->admin_cmd_queue);
        if (session_env->io_cmd_queue.queue_size != 0)
            polling_thread_fetch_cmd(&io_queue, &session_env->io_cmd_queue);
        ret = pthread_mutex_unlock(&session_env->cmd_queue_mtx);



    }
}