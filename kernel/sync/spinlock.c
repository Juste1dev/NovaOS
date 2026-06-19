#include "spinlock.h"

static inline uint32_t spin_exchange(volatile uint32_t *ptr, uint32_t value) {
    __asm__ volatile("lock xchg %0, %1" : "+r"(value), "+m"(*ptr) : : "memory");
    return value;
}

void spinlock_init(spinlock_t *lock, const char *name) {
    if (!lock) return;
    lock->value = 0;
    lock->name = name;
}

int spin_try_lock(spinlock_t *lock) {
    if (!lock) return 0;
    return spin_exchange(&lock->value, 1) == 0;
}

void spin_lock(spinlock_t *lock) {
    if (!lock) return;
    while (!spin_try_lock(lock)) {
        while (lock->value) __asm__ volatile("pause");
    }
}

void spin_unlock(spinlock_t *lock) {
    if (!lock) return;
    __asm__ volatile("" ::: "memory");
    lock->value = 0;
}
