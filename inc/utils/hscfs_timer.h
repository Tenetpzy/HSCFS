#pragma once

#include <stdint.h>

typedef struct hscfs_timer hscfs_timer;
typedef struct hscfs_timer_monitor hscfs_timer_monitor;
typedef void (*hscfs_timer_cb)(uint64_t overflow_times, void *arg);

/* 定时器操作接口 */
hscfs_timer* new_hscfs_timer(void);
void hscfs_timer_set(hscfs_timer *self, struct timespec *expiration_time, uint8_t is_period);
int hscfs_timer_start(hscfs_timer *self);
int hscfs_timer_stop(hscfs_timer *self);
void free_hscfs_timer(hscfs_timer *self);

/* 
 * 定时器监控器操作接口
 * 监控器不具备对定时器的所有权，将定时器加入监控器后，销毁监控器不会销毁对应定时器
 */
hscfs_timer_monitor* new_hscfs_timer_monitor(void);
int hscfs_timer_monitor_add_timer(hscfs_timer_monitor *self, hscfs_timer *timer,
    hscfs_timer_cb cb_func, void *cb_arg);
int hscfs_timer_monitor_del_timer(hscfs_timer_monitor *self, hscfs_timer *timer);

// 等待已加入的定时器到期，该函数可能阻塞
int hscfs_timer_monitor_wait_added_timer(hscfs_timer_monitor *self);
