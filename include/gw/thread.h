// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023  Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */

#ifndef GNUWEEB__THREAD_H
#define GNUWEEB__THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdatomic.h>

#ifdef CONFIG_POSIX_THREAD
#include <pthread.h>
typedef pthread_t thread_t;
typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t cond_t;
#else /* #ifdef CONFIG_POSIX_THREAD */
#include <threads.h>
struct c11_thread;
typedef struct c11_thread *thread_t;
typedef mtx_t mutex_t;
typedef cnd_t cond_t;
#endif /* #ifdef CONFIG_POSIX_THREAD */

int thread_create(thread_t *thread, void *(*func)(void *), void *arg);
int thread_join(thread_t thread, void **retval);
int thread_detach(thread_t thread);
int thread_equal(thread_t t1, thread_t t2);

int mutex_init(mutex_t *mutex);
int mutex_destroy(mutex_t *mutex);
int mutex_lock(mutex_t *mutex);
int mutex_trylock(mutex_t *mutex);
int mutex_unlock(mutex_t *mutex);
int mutex_timedlock(mutex_t *mutex, const struct timespec *abstime);

int cond_init(cond_t *cond);
int cond_destroy(cond_t *cond);
int cond_wait(cond_t *cond, mutex_t *mutex);
int cond_timedwait(cond_t *cond, mutex_t *mutex, const struct timespec *abstime);
int cond_signal(cond_t *cond);
int cond_broadcast(cond_t *cond);
int cond_broadcast_n(cond_t *cond, uint32_t n);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* #ifndef GNUWEEB__THREAD_H */
