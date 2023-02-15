#pragma once

// 会话层环境初始化，需要在系统启动时调用
int comm_session_env_init(void);

// 将命令上下文提交给会话层，此后由轮询线程完成轮询该命令CQE等后续流程
int comm_session_env_submit_cmd_ctx(comm_session_cmd_ctx *cmd_ctx);

void comm_session_polling_thread_callback(comm_cmd_CQE_result result, void *arg);