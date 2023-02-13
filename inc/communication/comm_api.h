#pragma once

// 通信层接口头文件

#include <stdint.h>

typedef enum comm_cmd_result
{
    COMM_CMD_SUCCESS, COMM_CMD_CQE_ERROR, COMM_CMD_TRANS_ERROR
} comm_cmd_result;

// 通信层异步接口的回调函数
typedef void (*comm_async_cb_func)(comm_cmd_result, void *);

