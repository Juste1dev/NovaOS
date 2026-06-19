#include "pthread_runtime.h"
#include "../sched/scheduler.h"
#include "../timer.h"
#include "../../fs/vfs.h"
#include "../../libc.h"
#include <stdint.h>
#include <stddef.h>

#define NOVA_PTHREAD_MAX_THREADS 16
#define NOVA_PTHREAD_MAX_KEYS    16

typedef struct {
    int used;
    pthread_t tid;
    void *(*start_routine)(void*);
    void *arg;
    void *retval;
    int finished;
} nova_pthread_slot_t;

static nova_pthread_slot_t g_slots[NOVA_PTHREAD_MAX_THREADS];
static void *g_tls[NOVA_PTHREAD_MAX_THREADS][NOVA_PTHREAD_MAX_KEYS];
static int g_key_used[NOVA_PTHREAD_MAX_KEYS];
static uint32_t g_created = 0;
static uint32_t g_joined = 0;
static uint32_t g_signals = 0;
static int g_initialized = 0;

static int pt_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void pt_copy(char *dst, const char *src, int max) { int i = 0; if (!dst || max <= 0) return; while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; } dst[i] = 0; }
static void pt_cat(char *dst, const char *src, int max) { int dl = pt_len(dst), i = 0; if (!dst || !src || max <= 0 || dl >= max - 1) return; while (src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; } dst[dl + i] = 0; }
static void pt_u32(uint32_t value, char *buf, int max) { char tmp[16]; int pos = 0, out = 0; if (!buf || max <= 0) return; if (!value) { buf[0] = '0'; if (max > 1) buf[1] = 0; return; } while (value && pos < (int)sizeof(tmp)) { tmp[pos++] = (char)('0' + (value % 10u)); value /= 10u; } while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos]; buf[out] = 0; }
static int pt_slot_index(pthread_t tid) { for (int i = 0; i < NOVA_PTHREAD_MAX_THREADS; ++i) if (g_slots[i].used && g_slots[i].tid == tid) return i; return -1; }
static int pt_current_slot(void) { return pt_slot_index((pthread_t)scheduler_current_tid()); }
static void pt_publish(void) {
    char snap[2048];
    if (!vfs_exists("/proc")) return;
    pthread_runtime_snapshot(snap, sizeof(snap));
    (void)vfs_write_file("/proc/pthreads", snap, (uint32_t)pt_len(snap));
}

int pthread_runtime_init(void) {
    k_memset(g_slots, 0, sizeof(g_slots));
    k_memset(g_tls, 0, sizeof(g_tls));
    k_memset(g_key_used, 0, sizeof(g_key_used));
    g_created = 0; g_joined = 0; g_signals = 0; g_initialized = 1;
    pt_publish();
    return 1;
}

int pthread_create(pthread_t *thread, const void *attr, void *(*start_routine)(void*), void *arg) {
    int pid = scheduler_current_pid();
    int tid;
    (void)attr;
    if (!g_initialized || !thread || !start_routine) return -1;
    if (pid < 0) pid = scheduler_create_process("pthread-runtime", "/usr/lib/libpthread.so", NOVA_PRIORITY_NORMAL, NOVA_PROC_SERVICE | NOVA_PROC_USER);
    tid = scheduler_create_thread(pid, "pthread-worker", NOVA_PRIORITY_NORMAL, 0);
    if (tid < 0) return -1;
    for (int i = 0; i < NOVA_PTHREAD_MAX_THREADS; ++i) {
        if (!g_slots[i].used) {
            g_slots[i].used = 1;
            g_slots[i].tid = tid;
            g_slots[i].start_routine = start_routine;
            g_slots[i].arg = arg;
            g_slots[i].retval = NULL;
            g_slots[i].finished = 0;
            *thread = tid;
            ++g_created;
            pt_publish();
            return 0;
        }
    }
    return -1;
}

int pthread_join(pthread_t thread, void **retval) {
    int idx = pt_slot_index(thread);
    if (idx < 0) return -1;
    if (!g_slots[idx].finished && g_slots[idx].start_routine) {
        g_slots[idx].retval = g_slots[idx].start_routine(g_slots[idx].arg);
        g_slots[idx].finished = 1;
    }
    if (retval) *retval = g_slots[idx].retval;
    g_slots[idx].used = 0;
    ++g_joined;
    pt_publish();
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr) {
    (void)attr;
    if (!mutex) return -1;
    mutex_init(&mutex->native, "pthread-mutex");
    mutex->initialized = 1;
    return 0;
}
int pthread_mutex_lock(pthread_mutex_t *mutex) { if (!mutex || !mutex->initialized) return -1; mutex_lock(&mutex->native); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *mutex) { if (!mutex || !mutex->initialized) return -1; mutex_unlock(&mutex->native); return 0; }
int pthread_mutex_destroy(pthread_mutex_t *mutex) { if (!mutex) return -1; mutex->initialized = 0; return 0; }

int pthread_cond_init(pthread_cond_t *cond, const void *attr) {
    (void)attr;
    if (!cond) return -1;
    spinlock_init(&cond->guard, "pthread-cond");
    cond->sequence = 0;
    cond->initialized = 1;
    return 0;
}
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    uint32_t observed;
    uint32_t start;
    if (!cond || !mutex || !cond->initialized || !mutex->initialized) return -1;
    observed = cond->sequence;
    pthread_mutex_unlock(mutex);
    start = timer_ms();
    while (cond->sequence == observed && (timer_ms() - start) < 25) scheduler_yield();
    pthread_mutex_lock(mutex);
    return 0;
}
int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond || !cond->initialized) return -1;
    spin_lock(&cond->guard);
    cond->sequence++;
    spin_unlock(&cond->guard);
    ++g_signals;
    pt_publish();
    return 0;
}
int pthread_cond_broadcast(pthread_cond_t *cond) { return pthread_cond_signal(cond); }
int pthread_cond_destroy(pthread_cond_t *cond) { if (!cond) return -1; cond->initialized = 0; return 0; }

int pthread_key_create(pthread_key_t *key, void (*destructor)(void*)) {
    (void)destructor;
    if (!key) return -1;
    for (int i = 0; i < NOVA_PTHREAD_MAX_KEYS; ++i) {
        if (!g_key_used[i]) {
            g_key_used[i] = 1;
            *key = (pthread_key_t)i;
            pt_publish();
            return 0;
        }
    }
    return -1;
}
void *pthread_getspecific(pthread_key_t key) {
    int slot = pt_current_slot();
    if ((int)key < 0 || (int)key >= NOVA_PTHREAD_MAX_KEYS || slot < 0) return NULL;
    return g_tls[slot][key];
}
int pthread_setspecific(pthread_key_t key, const void *value) {
    int slot = pt_current_slot();
    if ((int)key < 0 || (int)key >= NOVA_PTHREAD_MAX_KEYS || slot < 0) return -1;
    g_tls[slot][key] = (void*)value;
    pt_publish();
    return 0;
}

void pthread_runtime_snapshot(char *buf, int max) {
    char nb[32];
    if (!buf || max <= 0) return;
    buf[0] = 0;
    pt_cat(buf, "Nova pthread runtime\n", max);
    pt_cat(buf, "apis=pthread_create,join,mutex,cond,TLS\n", max);
    pt_cat(buf, "threads.created=", max); pt_u32(g_created, nb, sizeof(nb)); pt_cat(buf, nb, max); pt_cat(buf, "\n", max);
    pt_cat(buf, "threads.joined=", max); pt_u32(g_joined, nb, sizeof(nb)); pt_cat(buf, nb, max); pt_cat(buf, "\n", max);
    pt_cat(buf, "cond.signals=", max); pt_u32(g_signals, nb, sizeof(nb)); pt_cat(buf, nb, max); pt_cat(buf, "\n", max);
    pt_cat(buf, "tls.keys=", max);
    {
        uint32_t keys = 0;
        for (int i = 0; i < NOVA_PTHREAD_MAX_KEYS; ++i) if (g_key_used[i]) keys++;
        pt_u32(keys, nb, sizeof(nb)); pt_cat(buf, nb, max); pt_cat(buf, "\n", max);
    }
}
