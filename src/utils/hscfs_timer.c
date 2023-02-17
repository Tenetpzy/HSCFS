#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>

#include "utils/queue_extras.h"
#include "utils/hscfs_log.h"
#include "utils/hscfs_timer.h"

typedef struct hscfs_timer
{
    int timer_fd;
    struct timespec _expiration_time;
    uint8_t _is_period;
} hscfs_timer;

hscfs_timer* new_hscfs_timer(void)
{
    hscfs_timer *timer = (hscfs_timer *)malloc(sizeof(hscfs_timer));
    if (timer == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc hscfs timer struct failed.");
        return NULL;
    }
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd == -1)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, errno, "timerfd create failed.");
        free(timer);
        return NULL;
    }
    timer->timer_fd = fd;
    return timer;
};

void free_hscfs_timer(hscfs_timer *self)
{
    int ret = close(self->timer_fd);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, errno, "timerfd close failed.");
    free(self);
}

void hscfs_timer_set(hscfs_timer *self, struct timespec *expiration_time, uint8_t is_period)
{
    self->_expiration_time = *expiration_time;
    self->_is_period = is_period;
}

int hscfs_timer_start(hscfs_timer *self)
{
    // 若itime.it_value为0，则解除定时器
    // 定时器到期后，将itime.it_interval装入it_value。若interval为0，则只启动1次
    struct itimerspec itime;
    itime.it_value = self->_expiration_time;
    if (self->_is_period)
        itime.it_interval = self->_expiration_time;
    int ret = timerfd_settime(self->timer_fd, 0, &itime, NULL);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, errno, "start hscfs timer failed.");
    return ret;
}

int hscfs_timer_stop(hscfs_timer *self)
{
    struct itimerspec itime;
    itime.it_value.tv_nsec = 0;
    itime.it_value.tv_sec = 0;
    int ret = timerfd_settime(self->timer_fd, 0, &itime, NULL);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, errno, "stop hscfs timer failed.");
    return ret;
}

#define HSCFS_TIMER_MONITOR_MAX_SIZE_PER_WAIT 16
#define HSCFS_TIMER_MONITOR_ENTRY_LIST_ENTRY list_entry

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

hscfs_timer_monitor* new_hscfs_timer_monitor(void)
{
    hscfs_timer_monitor *monitor = (hscfs_timer_monitor *)malloc(sizeof(hscfs_timer_monitor));
    if (monitor == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc timer monitor failed.");
        return NULL;
    }
    monitor->epoll_fd = epoll_create(10);  // 10是随便写的参数，目前内核不使用epoll_create的size hint
    if (monitor->epoll_fd == -1)
    {
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, errno, "timer monitor create epoll fd failed.");
        free(monitor);
        return NULL;
    }
    LIST_INIT(&monitor->monitor_entry_list);
    return monitor;
}

void free_hscfs_timer_monitor(hscfs_timer_monitor *self)
{
    int ret = close(self->epoll_fd);
    if (ret != 0)
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, errno, "close timer monitor epoll fd failed.");
    hscfs_timer_monitor_entry *entry, *nxt_entry;
    LIST_FOREACH_SAFE(entry, &self->monitor_entry_list, HSCFS_TIMER_MONITOR_ENTRY_LIST_ENTRY, nxt_entry)
    {
        LIST_REMOVE(entry, HSCFS_TIMER_MONITOR_ENTRY_LIST_ENTRY);
        free(entry);
    }
}

int hscfs_timer_monitor_add_timer(hscfs_timer_monitor *self, hscfs_timer *timer,
    hscfs_timer_cb cb_func, void *cb_arg)
{
    int ret = 0;
    hscfs_timer_monitor_entry *m_entry = (hscfs_timer_monitor_entry *)malloc(sizeof(hscfs_timer_monitor_entry));
    if (m_entry == NULL)
    {
        HSCFS_LOG(HSCFS_LOG_ERROR, "alloc timer monitor entry failed.");
        ret = ENOMEM;
        return ret;
    }
    struct epoll_event event = {.events = 0};
    event.events |= EPOLLIN;  // timerfd到期则可读
    event.data.ptr = m_entry;
    ret = epoll_ctl(self->epoll_fd, EPOLL_CTL_ADD, timer->timer_fd, &event);
    if (ret != 0)
    {
        ret = errno;
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "timer monitor add epoll failed.");
        free(m_entry);
        return ret;
    }

    m_entry->timer = timer;
    m_entry->cb_func = cb_func;
    m_entry->cb_arg = cb_arg;
    LIST_INSERT_HEAD(&self->monitor_entry_list, m_entry, HSCFS_TIMER_MONITOR_ENTRY_LIST_ENTRY);

    return 0;
}

int hscfs_timer_monitor_del_timer(hscfs_timer_monitor *self, hscfs_timer *timer)
{
    int ret = 0;
    ret = epoll_ctl(self->epoll_fd, EPOLL_CTL_DEL, timer->timer_fd, NULL);
    if (ret != 0)
    {
        ret = errno;
        HSCFS_ERRNO_LOG(HSCFS_LOG_WARNING, ret, "timer monitor del epoll failed.");
    }
    hscfs_timer_monitor_entry *entry, *nxt_entry;
    LIST_FOREACH_SAFE(entry, &self->monitor_entry_list, HSCFS_TIMER_MONITOR_ENTRY_LIST_ENTRY, nxt_entry)
    {
        if (entry->timer == timer)
        {
            LIST_REMOVE(entry, HSCFS_TIMER_MONITOR_ENTRY_LIST_ENTRY);
            free(entry);
            break;
        }
    }
    return ret;
}

int hscfs_timer_monitor_wait_added_timer(hscfs_timer_monitor *self)
{
    int ret = 0;
    while ((ret = epoll_wait(self->epoll_fd, self->evlist, HSCFS_TIMER_MONITOR_MAX_SIZE_PER_WAIT, -1)) == -1
        && errno == EINTR);
    if (ret == -1)
    {
        ret = errno;
        HSCFS_ERRNO_LOG(HSCFS_LOG_ERROR, ret, "timer monitor wait timer failed.");
        return ret;
    }

    // 为每个到期的定时器，调用其加入monitor时设置的回调函数
    for (int i = 0; i < ret; ++i)
    {
        assert(self->evlist[i].events & EPOLLIN);
        hscfs_timer_monitor_entry *m_entry = (hscfs_timer_monitor_entry *)self->evlist[i].data.ptr;
        uint64_t overflow_time;

        // 读出定时器溢出次数
        read(m_entry->timer->timer_fd, &overflow_time, sizeof(uint64_t));
        m_entry->cb_func(overflow_time, m_entry->cb_arg);
    }

    return ret;
}
