#ifndef NOVA_SPINLOCK_H
#define NOVA_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t value;
    const char *name;
} spinlock_t;

void spinlock_init(spinlock_t *lock, const char *name);
int  spin_try_lock(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

#endif
