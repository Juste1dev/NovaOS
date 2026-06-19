#ifndef NOVA_USERSPACE_H
#define NOVA_USERSPACE_H

#include <stdint.h>

#define USERSPACE_MAX_PROCS 10
#define USERSPACE_NAME_MAX  32
#define USERSPACE_ROLE_MAX  24
#define USERSPACE_STATE_MAX 24
#define USERSPACE_PATH_MAX  64
#define USERSPACE_FMT_MAX   16

typedef struct {
    int      pid;
    int      tid;
    int      uid;
    int      gid;
    int      session_id;
    int      ring;
    int      isolated;
    char     name[USERSPACE_NAME_MAX];
    char     role[USERSPACE_ROLE_MAX];
    char     state[USERSPACE_STATE_MAX];
    char     endpoint[USERSPACE_PATH_MAX];
    char     exec_format[USERSPACE_FMT_MAX];
    char     mailbox[USERSPACE_NAME_MAX];
    uint32_t started_ms;
    int      privileged;
    int      priority;
} userspace_process_t;

typedef struct {
    userspace_process_t processes[USERSPACE_MAX_PROCS];
    int      process_count;
    uint32_t boot_tick_ms;
    int      ready;
} userspace_state_t;

extern userspace_state_t g_userspace;

void userspace_init(void);
void userspace_publish_vfs(void);
void userspace_report(char *buf, int max);
void userspace_process_table(char *buf, int max);
int  userspace_process_count(void);

#endif
