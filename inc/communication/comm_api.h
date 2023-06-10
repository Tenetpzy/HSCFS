#pragma once

// 通信层接口头文件

#ifdef __cplusplus
extern "C" {
#endif

#include "communication/vendor_cmds.h"

typedef struct comm_dev comm_dev;
typedef struct comm_raw_cmd comm_raw_cmd;

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

int comm_submit_sync_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count, 
    comm_io_direction dir);
int comm_submit_async_rw_request(comm_dev *dev, void *buffer, uint64_t lba, uint32_t lba_count,
    comm_async_cb_func cb_func, void *cb_arg, comm_io_direction dir);

int comm_submit_sync_migrate_request(comm_dev *dev, migrate_task *task);
int comm_submit_async_migrate_request(comm_dev *dev, migrate_task *task, comm_async_cb_func cb_func, void *cb_arg);

int comm_submit_sync_path_lookup_request(comm_dev *dev, path_lookup_task *task, path_lookup_result *res);
int comm_submit_async_path_lookup_request(comm_dev *dev, path_lookup_task *task, path_lookup_result *res, 
    comm_async_cb_func cb_func, void *cb_arg);

int comm_submit_sync_filemapping_search_request(comm_dev *dev, filemapping_search_task *task, 
    void *res, uint32_t res_len);
int comm_submit_async_filemapping_search_request(comm_dev *dev, filemapping_search_task *task, 
    void *res, uint32_t res_len, comm_async_cb_func cb_func, void *cb_arg);

// 更新元数据日志尾指针命令。原位置是origin_lpa，新写入了write_block_num个block的日志
int comm_submit_sync_update_metajournal_tail_request(comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num);
int comm_submit_async_update_metajournal_tail_request(comm_dev *dev, uint64_t origin_lpa, uint32_t write_block_num,
    comm_async_cb_func cb_func, void *cb_arg);

// 获取元数据日志头指针命令。结果存放在head_lpa中，head_lpa必须是可DMA的内存。
// TODO : 需要修改，实际上用此命令同时获取头尾指针，不仅是头指针
int comm_submit_sync_get_metajournal_head_request(comm_dev *dev, uint64_t *head_lpa);
int comm_submit_async_get_metajournal_head_request(comm_dev *dev, uint64_t *head_lpa, 
    comm_async_cb_func cb_func, void *cb_arg);

// 文件系统模块初始化，此接口为同步接口
int comm_submit_fs_module_init_request(comm_dev *dev);

// 文件系统SSD DB区域初始化，同步接口
int comm_submit_fs_db_init_request(comm_dev *dev);

// 用DB内容恢复文件系统超级块，同步接口
int comm_submit_fs_recover_from_db_request(comm_dev *dev);

// 清空元数据日志，同步接口
int comm_submit_clear_metajournal_request(comm_dev *dev);

// 启动元数据日志应用，同步接口
int comm_submit_start_apply_journal_request(comm_dev *dev);

// 挂起元数据日志应用，同步接口
int comm_submit_stop_apply_journal_request(comm_dev *dev);

#ifdef __cplusplus
}
#endif