#pragma once

#include <stdint.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>

#include "utils/queue_extras.h"

typedef struct hscfs_timer
{
    int timer_fd;
    struct timespec _expiration_time;
    uint8_t _is_period;
    uint8_t _is_block_check;  // hscfs_timer_check_expire时是否阻塞直到到期
} hscfs_timer;

/* 定时器操作接口 */

/* is_block_check：定时器是阻塞还是非阻塞 */
int hscfs_timer_constructor(hscfs_timer *self, uint8_t is_block);
void hscfs_timer_destructor(hscfs_timer *self);
void hscfs_timer_set(hscfs_timer *self, struct timespec *expiration_time, uint8_t is_period);
int hscfs_timer_start(hscfs_timer *self);
int hscfs_timer_stop(hscfs_timer *self);

/*
 * 检查定时器是否到期。
 * 若定时器设置了_is_block_check，则阻塞到到期后返回0；若发生其它错误，返回errno。
 * 若定时器未设置_is_block_check，直接返回：若到期，返回0；若未到期，则返回EAGAIN；若发生其它错误，返回errno。
 * 若overflow_times不为NULL，则保存到期次数
 */
int hscfs_timer_check_expire(hscfs_timer *self, uint64_t *overflow_times);


/* 定时器监听器，可同时监听多个定时器 */

#define HSCFS_TIMER_MONITOR_MAX_SIZE_PER_WAIT 16
#define HSCFS_TIMER_MONITOR_ENTRY_LIST_ENTRY list_entry
typedef void (*hscfs_timer_cb)(uint64_t overflow_times, void *arg);

typedef struct hscfs_timer_monitor_entry
{
    hscfs_timer *timer;
    hscfs_timer_cb cb_func;
    void *cb_arg;
    LIST_ENTRY(hscfs_timer_monitor_entry) HSCFS_TIMER_MONITOR_ENTRY_LIST_ENTRY;
} hscfs_timer_monitor_entry;

typedef struct hscfs_timer_monitor
{
    int epoll_fd;
    struct epoll_event evlist[HSCFS_TIMER_MONITOR_MAX_SIZE_PER_WAIT];
    LIST_HEAD(, hscfs_timer_monitor_entry) monitor_entry_list;
} hscfs_timer_monitor;

/* 
 * 定时器监控器操作接口
 * 监控器不具备对定时器的所有权，将定时器加入监控器后，销毁监控器不会销毁对应定时器
 */
int hscfs_timer_monitor_constructor(hscfs_timer_monitor *self);
void hscfs_timer_monitor_destructor(hscfs_timer_monitor *self);
int hscfs_timer_monitor_add_timer(hscfs_timer_monitor *self, hscfs_timer *timer,
    hscfs_timer_cb cb_func, void *cb_arg);
int hscfs_timer_monitor_del_timer(hscfs_timer_monitor *self, hscfs_timer *timer);

/* 
 * 等待已加入的定时器到期，该函数可能阻塞
 * 返回已到期定时器个数，出错返回-errno
 */
int hscfs_timer_monitor_wait_added_timer(hscfs_timer_monitor *self);
