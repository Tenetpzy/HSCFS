#pragma once

// 用户态、内核多线程同步和互斥工具的兼容层

// 用户态
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
typedef pthread_spinlock_t spinlock_t;
typedef pthread_cond_t cond_t;

static int mutex_init(mutex_t *self)
{
    return pthread_mutex_init(self, NULL); 
}

static int mutex_lock(mutex_t *self)
{
    return pthread_mutex_lock(self);
}

static int mutex_trylock(mutex_t *self)
{
    return pthread_mutex_trylock(self);
}

static int mutex_unlock(mutex_t *self)
{
    return pthread_mutex_unlock(self);
}

static int mutex_destroy(mutex_t *self)
{
    return pthread_mutex_destroy(self);
}

static int spin_init(spinlock_t *self)
{
    return pthread_spin_init(self, PTHREAD_PROCESS_PRIVATE); 
}

static int spin_lock(spinlock_t *self)
{
    return pthread_spin_lock(self);
}

static int spin_unlock(spinlock_t *self)
{
    return pthread_spin_unlock(self);
}

static int spin_destroy(spinlock_t *self)
{
    return pthread_spin_destroy(self);
}

static int cond_init(cond_t *self)
{
    return pthread_cond_init(self, NULL);
}

static int cond_wait(cond_t *self, mutex_t *mtx)
{
    return pthread_cond_wait(self, mtx);
}

static int cond_signal(cond_t *self)
{
    return pthread_cond_signal(self);
}

static int cond_broadcast(cond_t *self)
{
    return pthread_cond_broadcast(self);
}