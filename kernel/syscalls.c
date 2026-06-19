#include "syscalls.h"
#include "sched/scheduler.h"
#include "memory.h"
#include "timer.h"
#include "ipc/ipc.h"
#include "elf/elf.h"
#include "users.h"
#include "../libc.h"
#include <stddef.h>

typedef struct {
    const char *name;
    const char *signature;
    nova_syscall_handler_t handler;
} syscall_entry_t;

static syscall_entry_t g_syscalls[NOVA_SYS_MAX];

static int sc_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void sc_cat(char *dst, const char *src, int max) {
    int dl;
    int i = 0;
    if (!dst || !src || max <= 0) return;
    dl = sc_len(dst);
    if (dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void sc_itoa(uint64_t value, char *buf, int max) {
    char tmp[24];
    int pos = 0;
    int out = 0;
    if (!buf || max <= 0) return;
    if (!value) {
        buf[0] = '0';
        if (max > 1) buf[1] = 0;
        return;
    }
    while (value && pos < (int)sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}

static const char *sc_str(uint64_t ptr) {
    return (const char *)(uintptr_t)ptr;
}

static char *sc_buf(uint64_t ptr) {
    return (char *)(uintptr_t)ptr;
}

static uint64_t sc_getpid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return (uint64_t)scheduler_current_pid();
}

static uint64_t sc_yield(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    scheduler_yield();
    return 0;
}

static uint64_t sc_uptime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return timer_ms();
}

static uint64_t sc_process_count(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return (uint64_t)scheduler_process_count();
}

static uint64_t sc_thread_count(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return (uint64_t)scheduler_thread_count();
}

static uint64_t sc_heap_used(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return heap_used();
}

static uint64_t sc_ipc_send(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a3;
    return (uint64_t)ipc_send(scheduler_current_pid(), (int)a0, sc_str(a1), (uint32_t)a2);
}

static uint64_t sc_ipc_recv(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a2; (void)a3;
    return (uint64_t)ipc_recv(scheduler_current_pid(), sc_buf(a0), (uint32_t)a1, NULL);
}

static uint64_t sc_pipe_create(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1; (void)a2; (void)a3;
    return (uint64_t)ipc_pipe_create(scheduler_current_pid(), sc_str(a0));
}

static uint64_t sc_pipe_write(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a3;
    return (uint64_t)ipc_pipe_write((int)a0, sc_str(a1), (uint32_t)a2);
}

static uint64_t sc_pipe_read(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a3;
    return (uint64_t)ipc_pipe_read((int)a0, sc_buf(a1), (uint32_t)a2);
}

static uint64_t sc_shm_create(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a2; (void)a3;
    return (uint64_t)ipc_shm_create(scheduler_current_pid(), sc_str(a0), (uint32_t)a1);
}

static uint64_t sc_shm_write(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a3;
    return (uint64_t)ipc_shm_write((int)a0, sc_str(a1), (uint32_t)a2);
}

static uint64_t sc_shm_read(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a3;
    return (uint64_t)ipc_shm_read((int)a0, sc_buf(a1), (uint32_t)a2);
}

static uint64_t sc_getuid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    const nova_process_t *proc;
    (void)a0; (void)a1; (void)a2; (void)a3;
    proc = scheduler_get_process(scheduler_current_pid());
    if (proc) return (uint64_t)proc->uid;
    if (users_get_current()) return (uint64_t)users_get_current()->uid;
    return (uint64_t)-1;
}

static uint64_t sc_fork(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1; (void)a2; (void)a3;
    return (uint64_t)scheduler_fork_current(sc_str(a0));
}

static uint64_t sc_exec(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    nova_elf_image_t img;
    const nova_process_t *proc;
    (void)a1; (void)a2; (void)a3;
    if (!sc_str(a0)) return (uint64_t)-1;
    if (!elf_load_from_vfs(sc_str(a0), &img)) return (uint64_t)-1;
    proc = scheduler_get_process(scheduler_current_pid());
    if (!proc) return (uint64_t)-1;
    return scheduler_exec_process(proc->pid, sc_str(a0), img.entry_point, img.user_stack_top, proc->ring, proc->priority) ? 0 : (uint64_t)-1;
}

static uint64_t sc_signal(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a2; (void)a3;
    return scheduler_signal_process((int)a0, (uint32_t)a1) ? 0 : (uint64_t)-1;
}

static uint64_t sc_session_id(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    const nova_process_t *proc;
    (void)a0; (void)a1; (void)a2; (void)a3;
    proc = scheduler_get_process(scheduler_current_pid());
    if (proc) return (uint64_t)proc->session_id;
    return (uint64_t)users_current_session_id();
}

int syscall_register(uint64_t num, const char *name, nova_syscall_handler_t handler) {
    if (num >= NOVA_SYS_MAX) return 0;
    g_syscalls[num].name = name;
    g_syscalls[num].handler = handler;
    return 1;
}

static void syscall_set_signature(uint64_t num, const char *signature) {
    if (num < NOVA_SYS_MAX) g_syscalls[num].signature = signature;
}

void syscall_init(void) {
    k_memset(g_syscalls, 0, sizeof(g_syscalls));
    syscall_register(NOVA_SYS_GETPID, "getpid", sc_getpid); syscall_set_signature(NOVA_SYS_GETPID, "getpid() -> pid");
    syscall_register(NOVA_SYS_YIELD, "yield", sc_yield); syscall_set_signature(NOVA_SYS_YIELD, "yield() -> 0");
    syscall_register(NOVA_SYS_UPTIME_MS, "uptime_ms", sc_uptime); syscall_set_signature(NOVA_SYS_UPTIME_MS, "uptime_ms() -> ms");
    syscall_register(NOVA_SYS_PROCESS_COUNT, "process_count", sc_process_count); syscall_set_signature(NOVA_SYS_PROCESS_COUNT, "process_count() -> n");
    syscall_register(NOVA_SYS_THREAD_COUNT, "thread_count", sc_thread_count); syscall_set_signature(NOVA_SYS_THREAD_COUNT, "thread_count() -> n");
    syscall_register(NOVA_SYS_HEAP_USED, "heap_used", sc_heap_used); syscall_set_signature(NOVA_SYS_HEAP_USED, "heap_used() -> bytes");
    syscall_register(NOVA_SYS_IPC_SEND, "ipc_send", sc_ipc_send); syscall_set_signature(NOVA_SYS_IPC_SEND, "ipc_send(dst_pid, payload, len) -> bytes");
    syscall_register(NOVA_SYS_IPC_RECV, "ipc_recv", sc_ipc_recv); syscall_set_signature(NOVA_SYS_IPC_RECV, "ipc_recv(buf, len) -> bytes");
    syscall_register(NOVA_SYS_PIPE_CREATE, "pipe_create", sc_pipe_create); syscall_set_signature(NOVA_SYS_PIPE_CREATE, "pipe_create(name) -> pipe_id");
    syscall_register(NOVA_SYS_PIPE_WRITE, "pipe_write", sc_pipe_write); syscall_set_signature(NOVA_SYS_PIPE_WRITE, "pipe_write(pipe_id, data, len) -> bytes");
    syscall_register(NOVA_SYS_PIPE_READ, "pipe_read", sc_pipe_read); syscall_set_signature(NOVA_SYS_PIPE_READ, "pipe_read(pipe_id, buf, len) -> bytes");
    syscall_register(NOVA_SYS_SHM_CREATE, "shm_create", sc_shm_create); syscall_set_signature(NOVA_SYS_SHM_CREATE, "shm_create(name, size) -> shm_id");
    syscall_register(NOVA_SYS_SHM_WRITE, "shm_write", sc_shm_write); syscall_set_signature(NOVA_SYS_SHM_WRITE, "shm_write(shm_id, data, len) -> bytes");
    syscall_register(NOVA_SYS_SHM_READ, "shm_read", sc_shm_read); syscall_set_signature(NOVA_SYS_SHM_READ, "shm_read(shm_id, buf, len) -> bytes");
    syscall_register(NOVA_SYS_GETUID, "getuid", sc_getuid); syscall_set_signature(NOVA_SYS_GETUID, "getuid() -> uid");
    syscall_register(NOVA_SYS_FORK, "fork", sc_fork); syscall_set_signature(NOVA_SYS_FORK, "fork(child_name) -> child_pid");
    syscall_register(NOVA_SYS_EXEC, "exec", sc_exec); syscall_set_signature(NOVA_SYS_EXEC, "exec(path) -> 0|-1");
    syscall_register(NOVA_SYS_SIGNAL, "signal", sc_signal); syscall_set_signature(NOVA_SYS_SIGNAL, "signal(pid, mask) -> 0|-1");
    syscall_register(NOVA_SYS_SESSION_ID, "session_id", sc_session_id); syscall_set_signature(NOVA_SYS_SESSION_ID, "session_id() -> sid");
}

uint64_t syscall_dispatch(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    if (num >= NOVA_SYS_MAX || !g_syscalls[num].handler) return (uint64_t)-1;
    return g_syscalls[num].handler(arg0, arg1, arg2, arg3);
}

const char* syscall_name(uint64_t num) {
    if (num >= NOVA_SYS_MAX || !g_syscalls[num].name) return "unknown";
    return g_syscalls[num].name;
}

void syscall_describe(char *buf, int max) {
    char nb[24];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    sc_cat(buf, "Nova syscall API\n", max);
    sc_cat(buf, "NUM NAME SIGNATURE\n", max);
    sc_cat(buf, "------------------\n", max);
    for (uint64_t i = 0; i < NOVA_SYS_MAX; ++i) {
        if (!g_syscalls[i].handler) continue;
        sc_itoa(i, nb, sizeof(nb));
        sc_cat(buf, nb, max);
        sc_cat(buf, " ", max);
        sc_cat(buf, g_syscalls[i].name, max);
        sc_cat(buf, " ", max);
        sc_cat(buf, g_syscalls[i].signature ? g_syscalls[i].signature : "-", max);
        sc_cat(buf, "\n", max);
    }
}
