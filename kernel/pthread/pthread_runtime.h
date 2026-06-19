#ifndef NOVA_PTHREAD_RUNTIME_H
#define NOVA_PTHREAD_RUNTIME_H

#include <stdint.h>
#include "../sync/mutex.h"
#include "../sync/spinlock.h"

typedef int pthread_t;
typedef unsigned int pthread_key_t;

typedef struct {
    mutex_t native;
    int initialized;
} pthread_mutex_t;

typedef struct {
    spinlock_t guard;
    volatile uint32_t sequence;
    int initialized;
} pthread_cond_t;

int pthread_runtime_init(void);
int pthread_create(pthread_t *thread, const void *attr, void *(*start_routine)(void*), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_cond_init(pthread_cond_t *cond, const void *attr);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_key_create(pthread_key_t *key, void (*destructor)(void*));
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void pthread_runtime_snapshot(char *buf, int max);

#endif
