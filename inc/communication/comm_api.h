#pragma once

// 通信层接口头文件

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "fs/fs.h"

typedef struct comm_dev comm_dev;
typedef struct comm_raw_cmd comm_raw_cmd;
struct migrate_task
{
    uint32_t migrateLpaCnt;
    struct f2fs_sit_entry victimSegInfo;
    uint64_t migrateDstLpa;
    uint64_t migrateSrcLpa;
}__attribute__((packed));
typedef struct migrate_task migrate_task;

typedef enum comm_cmd_result
{
    COMM_CMD_SUCCESS, COMM_CMD_CQE_ERROR, COMM_CMD_TID_QUERY_ERROR
} comm_cmd_result;

// 通信层异步接口的回调函数
typedef void (*comm_async_cb_func)(comm_cmd_result, void *);

int comm_submit_async_read_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count,
    comm_async_cb_func cb_func, void *cb_arg);

int comm_submit_sync_read_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count);

int comm_submit_raw_sync_cmd(comm_dev *dev, void *buf, uint32_t buf_len, comm_raw_cmd *raw_cmd);

#ifdef __cplusplus
}
#endif