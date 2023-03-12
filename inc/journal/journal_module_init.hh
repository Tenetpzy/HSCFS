#pragma once

#include <cstdint>

/*
 * 日志管理层初始化
 * 初始化日志提交队列
 * 启动日志处理线程
 * 
 * 参数：
 * SSD日志区域范围[journal_start_lpa, journal_end_lpa)。
 * 故障恢复完成后，SSD日志FIFO首地址journal_fifo_pos（此时FIFO尾地址也应是journal_fifo_pos，否则还有未重放的日志）
 */
void hscfs_journal_module_init(uint64_t journal_start_lpa, uint64_t journal_end_lpa, uint64_t journal_fifo_pos);