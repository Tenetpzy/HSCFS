#include "communication/session.h"
#include "utils/sys_log.h"

// 初始化同步命令的条件变量相关信息
static int comm_session_cmd_ctx_init_cond_info(comm_session_cmd_ctx *ctx)
{
    int ret = 0;
    ctx->cmd_is_cplt = 0;
    ret = pthread_cond_init(&ctx->wait_cond, NULL);
    if (ret != 0)
    {
        HSCFS_LOG_ERRNO(HSCFS_LOG_ERROR, ret, "init session cmd ctx condvar failed.");
        return ret;
    }
    ret = pthread_mutex_init(&ctx->wait_mtx, NULL);

    err1:
    int res = pthread_cond_destroy(&ctx->wait_cond);
    

}

comm_session_cmd_ctx* new_session_sync_cmd_ctx(comm_session_cmd_nvme_type cmd_type, int *rc)
{
    
}