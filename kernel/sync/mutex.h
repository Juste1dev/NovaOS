#ifndef NOVA_MUTEX_H
#define NOVA_MUTEX_H

#include <stdint.h>
#include "spinlock.h"

typedef struct {
    spinlock_t guard;
    volatile uint32_t locked;
    int owner_tid;
    uint32_t contention_count;
    uint32_t waiters;
    const char *name;
} mutex_t;

void mutex_init(mutex_t *mutex, const char *name);
int  mutex_try_lock(mutex_t *mutex);
void mutex_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);

#endif
