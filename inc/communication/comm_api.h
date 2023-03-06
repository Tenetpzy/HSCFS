#pragma once

// 通信层接口头文件

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct comm_dev comm_dev;

typedef enum comm_cmd_result
{
    COMM_CMD_SUCCESS, COMM_CMD_CQE_ERROR, COMM_CMD_TID_QUERY_ERROR
} comm_cmd_result;

// 通信层异步接口的回调函数
typedef void (*comm_async_cb_func)(comm_cmd_result, void *);

int comm_submit_async_read_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count,
    comm_async_cb_func cb_func, void *cb_arg);

int comm_submit_sync_read_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count);


#ifdef __cplusplus
}
#endif