#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 用户态、内核多线程同步和互斥工具的兼容层

// 用户态
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
typedef pthread_spinlock_t spinlock_t;
typedef pthread_cond_t cond_t;
typedef pthread_rwlock_t rwlock_t;

__attribute__((unused)) static int mutex_init(mutex_t *self)
{
    return pthread_mutex_init(self, NULL); 
}

__attribute__((unused)) static int mutex_lock(mutex_t *self)
{
    return pthread_mutex_lock(self);
}

__attribute__((unused)) static int mutex_trylock(mutex_t *self)
{
    return pthread_mutex_trylock(self);
}

__attribute__((unused)) static int mutex_unlock(mutex_t *self)
{
    return pthread_mutex_unlock(self);
}

__attribute__((unused)) static int mutex_destroy(mutex_t *self)
{
    return pthread_mutex_destroy(self);
}

__attribute__((unused)) static int spin_init(spinlock_t *self)
{
    return pthread_spin_init(self, PTHREAD_PROCESS_PRIVATE); 
}

__attribute__((unused)) static int spin_lock(spinlock_t *self)
{
    return pthread_spin_lock(self);
}

__attribute__((unused)) static int spin_unlock(spinlock_t *self)
{
    return pthread_spin_unlock(self);
}

__attribute__((unused)) static int spin_destroy(spinlock_t *self)
{
    return pthread_spin_destroy(self);
}

/* 默认读者优先 */
__attribute__((unused)) static int rwlock_init(rwlock_t *self)
{
    return pthread_rwlock_init(self, NULL);
}

/* 加共享读锁 */
__attribute__((unused)) static int rwlock_rdlock(rwlock_t *self)
{
    return pthread_rwlock_rdlock(self);
}

/* 加独占写锁 */
__attribute__((unused)) static int rwlock_wrlock(rwlock_t *self)
{
    return pthread_rwlock_wrlock(self);
}

__attribute__((unused)) static int rwlock_unlock(rwlock_t *self)
{
    return pthread_rwlock_unlock(self);
}

__attribute__((unused)) static int rwlock_destroy(rwlock_t *self)
{
    return pthread_rwlock_destroy(self);
}

__attribute__((unused)) static int cond_init(cond_t *self)
{
    return pthread_cond_init(self, NULL);
}

__attribute__((unused)) static int cond_wait(cond_t *self, mutex_t *mtx)
{
    return pthread_cond_wait(self, mtx);
}

__attribute__((unused)) static int cond_signal(cond_t *self)
{
    return pthread_cond_signal(self);
}

__attribute__((unused)) static int cond_broadcast(cond_t *self)
{
    return pthread_cond_broadcast(self);
}

__attribute__((unused)) static int cond_destroy(cond_t *self)
{
    return pthread_cond_destroy(self);
}


#ifdef __cplusplus
}
#endif