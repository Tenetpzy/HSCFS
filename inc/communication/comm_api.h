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

typedef enum comm_io_direction
{
    COMM_IO_READ, COMM_IO_WRITE
} comm_io_direction;

int comm_submit_async_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count,
    comm_async_cb_func cb_func, void *cb_arg, comm_io_direction dir);

int comm_submit_sync_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count, 
    comm_io_direction dir);

#ifdef __cplusplus
}
#endif