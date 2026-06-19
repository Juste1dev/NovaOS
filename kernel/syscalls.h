#ifndef NOVA_SYSCALLS_H
#define NOVA_SYSCALLS_H

#include <stdint.h>

typedef uint64_t (*nova_syscall_handler_t)(uint64_t, uint64_t, uint64_t, uint64_t);

enum {
    NOVA_SYS_GETPID = 0,
    NOVA_SYS_YIELD,
    NOVA_SYS_UPTIME_MS,
    NOVA_SYS_PROCESS_COUNT,
    NOVA_SYS_THREAD_COUNT,
    NOVA_SYS_HEAP_USED,
    NOVA_SYS_IPC_SEND,
    NOVA_SYS_IPC_RECV,
    NOVA_SYS_PIPE_CREATE,
    NOVA_SYS_PIPE_WRITE,
    NOVA_SYS_PIPE_READ,
    NOVA_SYS_SHM_CREATE,
    NOVA_SYS_SHM_WRITE,
    NOVA_SYS_SHM_READ,
    NOVA_SYS_GETUID,
    NOVA_SYS_FORK,
    NOVA_SYS_EXEC,
    NOVA_SYS_SIGNAL,
    NOVA_SYS_SESSION_ID,
    NOVA_SYS_MAX
};

void     syscall_init(void);
int      syscall_register(uint64_t num, const char *name, nova_syscall_handler_t handler);
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
const char* syscall_name(uint64_t num);
void     syscall_describe(char *buf, int max);

#endif
