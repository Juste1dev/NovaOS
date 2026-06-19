#ifndef NOVA_SCHEDULER_H
#define NOVA_SCHEDULER_H

#include <stdint.h>

#define NOVA_MAX_PROCESSES 16
#define NOVA_MAX_THREADS   32
#define NOVA_NAME_MAX      32
#define NOVA_PATH_MAX      64

#define NOVA_PROC_KERNEL   0x01
#define NOVA_PROC_SERVICE  0x02
#define NOVA_PROC_USER     0x04

#define NOVA_PRIORITY_IDLE    0
#define NOVA_PRIORITY_LOW     1
#define NOVA_PRIORITY_NORMAL  2
#define NOVA_PRIORITY_HIGH    3
#define NOVA_PRIORITY_RT      4

#define NOVA_RING_KERNEL      0
#define NOVA_RING_USER        3

#define NOVA_SIG_HUP   (1u << 0)
#define NOVA_SIG_INT   (1u << 1)
#define NOVA_SIG_TERM  (1u << 2)
#define NOVA_SIG_USR1  (1u << 3)
#define NOVA_SIG_USR2  (1u << 4)
#define NOVA_SIG_CHILD (1u << 5)
#define NOVA_SIG_PIPE  (1u << 6)
#define NOVA_SIG_ALRM  (1u << 7)

typedef enum {
    NOVA_TASK_UNUSED = 0,
    NOVA_TASK_READY,
    NOVA_TASK_RUNNING,
    NOVA_TASK_BLOCKED,
    NOVA_TASK_SLEEPING
} nova_task_state_t;

typedef struct {
    int pid;
    int parent_pid;
    char name[NOVA_NAME_MAX];
    char endpoint[NOVA_PATH_MAX];
    char image_path[NOVA_PATH_MAX];
    int priority;
    int flags;
    int main_tid;
    int thread_count;
    int uid;
    int gid;
    int session_id;
    int ring;
    int isolated;
    uint32_t runtime_ticks;
    uint32_t quantum_ticks;
    uint32_t exec_generation;
    uint32_t pending_signals;
    uint64_t entry_point;
    uint64_t user_stack_top;
    uint64_t address_space_id;
    uint64_t ipc_mailbox_id;
    uint64_t ipc_bytes;
    uint64_t shm_bytes;
    nova_task_state_t state;
} nova_process_t;

typedef struct {
    int tid;
    int pid;
    char name[NOVA_NAME_MAX];
    int priority;
    uint32_t runtime_ticks;
    uint32_t quantum_remaining;
    uint32_t switches;
    int kernel_thread;
    uint64_t user_stack_top;
    uint64_t vruntime;
    uint32_t sched_weight;
    uint32_t wakeup_granularity;
    uint32_t response_score;
    nova_task_state_t state;
} nova_thread_t;

void scheduler_init(void);
void scheduler_install_timer_hook(void);
int  scheduler_create_process(const char *name, const char *endpoint, int priority, int flags);
int  scheduler_create_thread(int pid, const char *name, int priority, int kernel_thread);
void scheduler_on_tick(void);
void scheduler_yield(void);
int  scheduler_current_pid(void);
int  scheduler_current_tid(void);
int  scheduler_process_count(void);
int  scheduler_thread_count(void);
int  scheduler_process_priority(int pid);
uint32_t scheduler_runtime_ticks(int pid);
const char* scheduler_process_state_string(int pid);
void scheduler_snapshot(char *buf, int max);
void scheduler_threads_snapshot(char *buf, int max);

void scheduler_set_process_security(int pid, int uid, int gid, int session_id, int ring, int isolated);
void scheduler_set_process_image(int pid, const char *path, uint64_t entry_point, uint64_t user_stack_top);
void scheduler_account_ipc(int pid, uint32_t ipc_bytes, uint32_t shm_bytes);
const nova_process_t* scheduler_get_process(int pid);
nova_process_t* scheduler_get_process_mut(int pid);
int  scheduler_fork_current(const char *child_name);
int  scheduler_exec_process(int pid, const char *path, uint64_t entry_point, uint64_t user_stack_top, int ring, int priority);
int  scheduler_signal_process(int pid, uint32_t signal_mask);
uint32_t scheduler_process_signals(int pid);

#endif
