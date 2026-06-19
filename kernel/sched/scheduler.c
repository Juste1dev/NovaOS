#include "scheduler.h"
#include "../sync/spinlock.h"
#include "../timer.h"
#include "../../libc.h"
#include <stddef.h>

static nova_process_t g_processes[NOVA_MAX_PROCESSES];
static nova_thread_t g_threads[NOVA_MAX_THREADS];
static int g_process_count = 0;
static int g_thread_count = 0;
static int g_current_tid = -1;
static int g_next_pid = 1;
static int g_next_tid = 1;
static uint32_t g_sched_ticks = 0;
static uint32_t g_preemptions = 0;
static uint64_t g_min_vruntime = 0;
static spinlock_t g_sched_lock;

static int sched_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void sched_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void sched_cat(char *dst, const char *src, int max) {
    int dl = sched_len(dst), i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = 0;
}
static void sched_itoa_u32(uint32_t value, char *buf, int max) {
    char tmp[16]; int pos = 0, out = 0;
    if (!buf || max <= 0) return;
    if (!value) { buf[0] = '0'; if (max > 1) buf[1] = 0; return; }
    while (value && pos < (int)sizeof(tmp)) { tmp[pos++] = (char)('0' + (value % 10u)); value /= 10u; }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}
static void sched_itoa_u64(uint64_t value, char *buf, int max) {
    char tmp[24]; int pos = 0, out = 0;
    if (!buf || max <= 0) return;
    if (!value) { buf[0] = '0'; if (max > 1) buf[1] = 0; return; }
    while (value && pos < (int)sizeof(tmp)) { tmp[pos++] = (char)('0' + (value % 10ULL)); value /= 10ULL; }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}
static const char *sched_ring_string(int ring) { return ring == NOVA_RING_USER ? "ring3" : "ring0"; }
static const char *sched_state_string(nova_task_state_t state) {
    switch (state) {
        case NOVA_TASK_READY: return "ready";
        case NOVA_TASK_RUNNING: return "running";
        case NOVA_TASK_BLOCKED: return "blocked";
        case NOVA_TASK_SLEEPING: return "sleep";
        default: return "unused";
    }
}
static const char *sched_basename(const char *path) {
    const char *base = path;
    if (!path) return "proc";
    for (const char *p = path; *p; ++p) if (*p == '/') base = p + 1;
    return base[0] ? base : path;
}
static nova_process_t *sched_find_process_locked(int pid) {
    for (int i = 0; i < g_process_count; ++i) if (g_processes[i].pid == pid) return &g_processes[i];
    return NULL;
}
static nova_thread_t *sched_find_thread_by_tid_locked(int tid) {
    for (int i = 0; i < g_thread_count; ++i) if (g_threads[i].tid == tid) return &g_threads[i];
    return NULL;
}
static uint32_t sched_quantum_for_priority(int priority) {
    switch (priority) {
        case NOVA_PRIORITY_RT: return 2;
        case NOVA_PRIORITY_HIGH: return 3;
        case NOVA_PRIORITY_NORMAL: return 4;
        case NOVA_PRIORITY_LOW: return 5;
        default: return 6;
    }
}
static uint32_t sched_weight_for_priority(int priority) {
    switch (priority) {
        case NOVA_PRIORITY_RT: return 2048;
        case NOVA_PRIORITY_HIGH: return 1536;
        case NOVA_PRIORITY_NORMAL: return 1024;
        case NOVA_PRIORITY_LOW: return 768;
        default: return 512;
    }
}
static uint64_t sched_vruntime_delta(uint32_t weight) {
    if (weight >= 2048) return 1;
    if (weight >= 1536) return 2;
    if (weight >= 1024) return 3;
    if (weight >= 768) return 4;
    return 5;
}
static uint64_t sched_ready_min_vruntime_locked(void) {
    uint64_t min = 0;
    int found = 0;
    for (int i = 0; i < g_thread_count; ++i) {
        nova_thread_t *t = &g_threads[i];
        if (t->state != NOVA_TASK_READY && t->state != NOVA_TASK_RUNNING) continue;
        if (!found || t->vruntime < min) { min = t->vruntime; found = 1; }
    }
    return found ? min : 0;
}
static int sched_pick_next_tid_locked(void) {
    int best_index = -1;
    uint64_t best_vruntime = 0;
    int start = 0;
    for (int i = 0; i < g_thread_count; ++i) {
        if (g_threads[i].tid == g_current_tid) { start = (i + 1) % g_thread_count; break; }
    }
    for (int off = 0; off < g_thread_count; ++off) {
        int idx = (start + off) % g_thread_count;
        nova_thread_t *t = &g_threads[idx];
        if (t->state != NOVA_TASK_READY && t->state != NOVA_TASK_RUNNING) continue;
        if (best_index < 0 || t->vruntime < best_vruntime || (t->vruntime == best_vruntime && t->priority > g_threads[best_index].priority)) {
            best_index = idx;
            best_vruntime = t->vruntime;
        }
    }
    return best_index >= 0 ? g_threads[best_index].tid : -1;
}
static void sched_switch_locked(int next_tid, int preemptive) {
    nova_thread_t *current;
    nova_thread_t *next;
    nova_process_t *proc;
    if (next_tid < 0) return;
    if (g_current_tid == next_tid) {
        next = sched_find_thread_by_tid_locked(next_tid);
        if (next) {
            uint32_t base = sched_quantum_for_priority(next->priority);
            next->state = NOVA_TASK_RUNNING;
            next->quantum_remaining = base + (next->response_score > 2 ? 0 : 1);
        }
        return;
    }
    current = sched_find_thread_by_tid_locked(g_current_tid);
    if (current && current->state == NOVA_TASK_RUNNING) current->state = NOVA_TASK_READY;
    next = sched_find_thread_by_tid_locked(next_tid);
    if (!next) return;
    next->state = NOVA_TASK_RUNNING;
    next->switches++;
    next->response_score = 0;
    next->quantum_remaining = sched_quantum_for_priority(next->priority);
    g_current_tid = next_tid;
    proc = sched_find_process_locked(next->pid);
    if (proc) proc->state = NOVA_TASK_RUNNING;
    g_min_vruntime = sched_ready_min_vruntime_locked();
    if (preemptive) g_preemptions++;
}

void scheduler_init(void) {
    k_memset(g_processes, 0, sizeof(g_processes));
    k_memset(g_threads, 0, sizeof(g_threads));
    g_process_count = 0; g_thread_count = 0; g_current_tid = -1; g_next_pid = 1; g_next_tid = 1; g_sched_ticks = 0; g_preemptions = 0; g_min_vruntime = 0;
    spinlock_init(&g_sched_lock, "scheduler");
    {
        int pid = scheduler_create_process("idle", "/kernel/idle", NOVA_PRIORITY_IDLE, NOVA_PROC_KERNEL);
        int tid = scheduler_create_thread(pid, "idle-main", NOVA_PRIORITY_IDLE, 1);
        g_current_tid = tid;
        g_threads[0].state = NOVA_TASK_RUNNING;
        g_processes[0].state = NOVA_TASK_RUNNING;
    }
}
void scheduler_install_timer_hook(void) { timer_register_callback(scheduler_on_tick); }
int scheduler_create_process(const char *name, const char *endpoint, int priority, int flags) {
    nova_process_t *proc; int pid; int parent_pid = scheduler_current_pid();
    spin_lock(&g_sched_lock);
    if (g_process_count >= NOVA_MAX_PROCESSES) { spin_unlock(&g_sched_lock); return -1; }
    proc = &g_processes[g_process_count++];
    k_memset(proc, 0, sizeof(*proc));
    pid = g_next_pid++;
    proc->pid = pid; proc->parent_pid = parent_pid > 0 ? parent_pid : -1; proc->priority = priority; proc->flags = flags; proc->state = NOVA_TASK_READY; proc->quantum_ticks = sched_quantum_for_priority(priority);
    proc->main_tid = -1; proc->uid = (flags & NOVA_PROC_KERNEL) ? 0 : 1000; proc->gid = (flags & NOVA_PROC_KERNEL) ? 0 : 1000; proc->session_id = (flags & NOVA_PROC_KERNEL) ? 0 : 1; proc->ring = (flags & NOVA_PROC_KERNEL) ? NOVA_RING_KERNEL : NOVA_RING_USER; proc->isolated = (flags & NOVA_PROC_USER) ? 1 : 0;
    proc->address_space_id = ((uint64_t)pid << 12) | 0x1000ULL; proc->entry_point = 0x0000000000400000ULL + ((uint64_t)pid * 0x10000ULL); proc->user_stack_top = 0x00007FFF00000000ULL - ((uint64_t)pid * 0x200000ULL);
    sched_copy(proc->name, name, sizeof(proc->name)); sched_copy(proc->endpoint, endpoint, sizeof(proc->endpoint)); sched_copy(proc->image_path, endpoint, sizeof(proc->image_path));
    spin_unlock(&g_sched_lock);
    return pid;
}
int scheduler_create_thread(int pid, const char *name, int priority, int kernel_thread) {
    nova_thread_t *thread; nova_process_t *proc; int tid;
    spin_lock(&g_sched_lock);
    if (g_thread_count >= NOVA_MAX_THREADS) { spin_unlock(&g_sched_lock); return -1; }
    proc = sched_find_process_locked(pid);
    if (!proc) { spin_unlock(&g_sched_lock); return -1; }
    thread = &g_threads[g_thread_count++];
    k_memset(thread, 0, sizeof(*thread));
    tid = g_next_tid++;
    thread->tid = tid; thread->pid = pid; thread->priority = priority; thread->kernel_thread = kernel_thread; thread->state = NOVA_TASK_READY; thread->quantum_remaining = sched_quantum_for_priority(priority);
    thread->user_stack_top = kernel_thread ? 0 : (proc->user_stack_top - ((uint64_t)proc->thread_count * 0x10000ULL));
    thread->sched_weight = sched_weight_for_priority(priority);
    thread->wakeup_granularity = sched_quantum_for_priority(priority);
    thread->vruntime = g_min_vruntime;
    sched_copy(thread->name, name, sizeof(thread->name));
    proc->thread_count++;
    if (proc->main_tid < 0) proc->main_tid = tid;
    spin_unlock(&g_sched_lock);
    return tid;
}
void scheduler_on_tick(void) {
    spin_lock(&g_sched_lock);
    g_sched_ticks++;
    {
        nova_thread_t *current = sched_find_thread_by_tid_locked(g_current_tid);
        if (current) {
            current->runtime_ticks++;
            current->response_score++;
            current->vruntime += sched_vruntime_delta(current->sched_weight);
            if (current->quantum_remaining) current->quantum_remaining--;
            {
                nova_process_t *proc = sched_find_process_locked(current->pid);
                if (proc) proc->runtime_ticks++;
            }
            g_min_vruntime = sched_ready_min_vruntime_locked();
            if (current->quantum_remaining == 0 || current->vruntime > (g_min_vruntime + current->wakeup_granularity)) {
                int next_tid = sched_pick_next_tid_locked();
                sched_switch_locked(next_tid, 1);
            }
        }
    }
    spin_unlock(&g_sched_lock);
}
void scheduler_yield(void) { spin_lock(&g_sched_lock); sched_switch_locked(sched_pick_next_tid_locked(), 0); spin_unlock(&g_sched_lock); }
int scheduler_current_pid(void) { nova_thread_t *thread = sched_find_thread_by_tid_locked(g_current_tid); return thread ? thread->pid : -1; }
int scheduler_current_tid(void) { return g_current_tid; }
int scheduler_process_count(void) { return g_process_count; }
int scheduler_thread_count(void) { return g_thread_count; }
int scheduler_process_priority(int pid) { nova_process_t *proc = sched_find_process_locked(pid); return proc ? proc->priority : -1; }
uint32_t scheduler_runtime_ticks(int pid) { nova_process_t *proc = sched_find_process_locked(pid); return proc ? proc->runtime_ticks : 0; }
const char* scheduler_process_state_string(int pid) { nova_process_t *proc = sched_find_process_locked(pid); return proc ? sched_state_string(proc->state) : "missing"; }
void scheduler_set_process_security(int pid, int uid, int gid, int session_id, int ring, int isolated) { spin_lock(&g_sched_lock); { nova_process_t *proc = sched_find_process_locked(pid); if (proc) { proc->uid = uid; proc->gid = gid; proc->session_id = session_id; proc->ring = ring; proc->isolated = isolated; } } spin_unlock(&g_sched_lock); }
void scheduler_set_process_image(int pid, const char *path, uint64_t entry_point, uint64_t user_stack_top) { spin_lock(&g_sched_lock); { nova_process_t *proc = sched_find_process_locked(pid); if (proc) { sched_copy(proc->image_path, path, sizeof(proc->image_path)); proc->entry_point = entry_point; proc->user_stack_top = user_stack_top; } } spin_unlock(&g_sched_lock); }
void scheduler_account_ipc(int pid, uint32_t ipc_bytes, uint32_t shm_bytes) { spin_lock(&g_sched_lock); { nova_process_t *proc = sched_find_process_locked(pid); if (proc) { proc->ipc_bytes += ipc_bytes; proc->shm_bytes += shm_bytes; } } spin_unlock(&g_sched_lock); }
const nova_process_t* scheduler_get_process(int pid) { return sched_find_process_locked(pid); }
nova_process_t* scheduler_get_process_mut(int pid) { return sched_find_process_locked(pid); }
int scheduler_fork_current(const char *child_name) {
    nova_process_t *parent = sched_find_process_locked(scheduler_current_pid()); nova_process_t *child; int child_pid; int child_tid; char thread_name[NOVA_NAME_MAX];
    if (!parent) return -1;
    child_pid = scheduler_create_process(child_name && child_name[0] ? child_name : parent->name, parent->endpoint, parent->priority, parent->flags);
    if (child_pid < 0) return -1;
    child = scheduler_get_process_mut(child_pid);
    if (!child) return -1;
    child->parent_pid = parent->pid; child->uid = parent->uid; child->gid = parent->gid; child->session_id = parent->session_id; child->ring = parent->ring; child->isolated = parent->isolated; child->entry_point = parent->entry_point; child->user_stack_top = parent->user_stack_top - 0x10000ULL; child->address_space_id = parent->address_space_id + 0x1000ULL; child->pending_signals = 0;
    sched_copy(child->image_path, parent->image_path, sizeof(child->image_path)); sched_copy(thread_name, child->name, sizeof(thread_name)); sched_cat(thread_name, "-main", sizeof(thread_name));
    child_tid = scheduler_create_thread(child_pid, thread_name, parent->priority, parent->ring == NOVA_RING_KERNEL);
    if (child_tid < 0) return -1;
    child->state = NOVA_TASK_READY; scheduler_signal_process(parent->pid, NOVA_SIG_CHILD); return child_pid;
}
int scheduler_exec_process(int pid, const char *path, uint64_t entry_point, uint64_t user_stack_top, int ring, int priority) {
    spin_lock(&g_sched_lock);
    {
        nova_process_t *proc = sched_find_process_locked(pid);
        if (!proc) { spin_unlock(&g_sched_lock); return 0; }
        sched_copy(proc->endpoint, path, sizeof(proc->endpoint)); sched_copy(proc->image_path, path, sizeof(proc->image_path)); sched_copy(proc->name, sched_basename(path), sizeof(proc->name));
        proc->entry_point = entry_point; proc->user_stack_top = user_stack_top; proc->ring = ring; proc->priority = priority; proc->exec_generation++; proc->state = NOVA_TASK_READY;
        if (proc->main_tid >= 0) {
            nova_thread_t *thread = sched_find_thread_by_tid_locked(proc->main_tid);
            if (thread) {
                sched_copy(thread->name, proc->name, sizeof(thread->name)); thread->priority = priority; thread->user_stack_top = user_stack_top; thread->state = NOVA_TASK_READY; thread->quantum_remaining = sched_quantum_for_priority(priority); thread->sched_weight = sched_weight_for_priority(priority); thread->wakeup_granularity = sched_quantum_for_priority(priority);
            }
        }
    }
    spin_unlock(&g_sched_lock);
    return 1;
}
int scheduler_signal_process(int pid, uint32_t signal_mask) { int ok = 0; spin_lock(&g_sched_lock); { nova_process_t *proc = sched_find_process_locked(pid); if (proc) { proc->pending_signals |= signal_mask; ok = 1; } } spin_unlock(&g_sched_lock); return ok; }
uint32_t scheduler_process_signals(int pid) { nova_process_t *proc = sched_find_process_locked(pid); return proc ? proc->pending_signals : 0; }
void scheduler_snapshot(char *buf, int max) {
    char nb[32];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    sched_cat(buf, "Scheduler Nova\n", max);
    sched_cat(buf, "mode=CFS-preemptive\n", max);
    sched_cat(buf, "PID PRI UID SID RING ISO SIG ASID IMG\n", max);
    sched_cat(buf, "--------------------------------------------------------------\n", max);
    for (int i = 0; i < g_process_count; ++i) {
        nova_process_t *proc = &g_processes[i];
        char line[256];
        k_memset(line, 0, sizeof(line));
        sched_itoa_u32((uint32_t)proc->pid, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u32((uint32_t)proc->priority, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u32((uint32_t)proc->uid, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u32((uint32_t)proc->session_id, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_cat(line, sched_ring_string(proc->ring), sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_cat(line, proc->isolated ? "yes" : "no", sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u32(proc->pending_signals, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u64(proc->address_space_id, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_cat(line, proc->image_path[0] ? proc->image_path : proc->endpoint, sizeof(line)); sched_cat(line, "\n", sizeof(line));
        sched_cat(buf, line, max);
    }
    sched_cat(buf, "\npreemptions=", max); sched_itoa_u32(g_preemptions, nb, sizeof(nb)); sched_cat(buf, nb, max);
    sched_cat(buf, " ticks=", max); sched_itoa_u32(g_sched_ticks, nb, sizeof(nb)); sched_cat(buf, nb, max);
    sched_cat(buf, " threads=", max); sched_itoa_u32((uint32_t)g_thread_count, nb, sizeof(nb)); sched_cat(buf, nb, max);
    sched_cat(buf, " min_vruntime=", max); sched_itoa_u64(g_min_vruntime, nb, sizeof(nb)); sched_cat(buf, nb, max);
    sched_cat(buf, "\n", max);
}

void scheduler_threads_snapshot(char *buf, int max) {
    char nb[32];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    sched_cat(buf, "Threads Nova\n", max);
    sched_cat(buf, "TID PID PRI STATE RUNTIME SWITCHES VRUNTIME QUANTUM NAME\n", max);
    sched_cat(buf, "----------------------------------------------------------------\n", max);
    for (int i = 0; i < g_thread_count; ++i) {
        nova_thread_t *thread = &g_threads[i];
        char line[256];
        k_memset(line, 0, sizeof(line));
        sched_itoa_u32((uint32_t)thread->tid, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u32((uint32_t)thread->pid, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u32((uint32_t)thread->priority, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_cat(line, sched_state_string(thread->state), sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u32(thread->runtime_ticks, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u32(thread->switches, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u64(thread->vruntime, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_itoa_u32(thread->quantum_remaining, nb, sizeof(nb)); sched_cat(line, nb, sizeof(line)); sched_cat(line, "   ", sizeof(line));
        sched_cat(line, thread->name, sizeof(line)); sched_cat(line, "\n", sizeof(line));
        sched_cat(buf, line, max);
    }
}
