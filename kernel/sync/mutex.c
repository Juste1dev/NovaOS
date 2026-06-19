#include "mutex.h"
#include "../sched/scheduler.h"

void mutex_init(mutex_t *mutex, const char *name) {
    if (!mutex) return;
    spinlock_init(&mutex->guard, name);
    mutex->locked = 0;
    mutex->owner_tid = -1;
    mutex->contention_count = 0;
    mutex->waiters = 0;
    mutex->name = name;
}

int mutex_try_lock(mutex_t *mutex) {
    int ok = 0;
    if (!mutex) return 0;
    spin_lock(&mutex->guard);
    if (!mutex->locked) {
        mutex->locked = 1;
        mutex->owner_tid = scheduler_current_tid();
        ok = 1;
    }
    spin_unlock(&mutex->guard);
    return ok;
}

void mutex_lock(mutex_t *mutex) {
    if (!mutex) return;
    while (!mutex_try_lock(mutex)) {
        spin_lock(&mutex->guard);
        mutex->waiters++;
        mutex->contention_count++;
        spin_unlock(&mutex->guard);
        scheduler_yield();
        __asm__ volatile("pause");
        spin_lock(&mutex->guard);
        if (mutex->waiters) mutex->waiters--;
        spin_unlock(&mutex->guard);
    }
}

void mutex_unlock(mutex_t *mutex) {
    if (!mutex) return;
    spin_lock(&mutex->guard);
    if (mutex->locked && mutex->owner_tid == scheduler_current_tid()) {
        mutex->locked = 0;
        mutex->owner_tid = -1;
    }
    spin_unlock(&mutex->guard);
}
