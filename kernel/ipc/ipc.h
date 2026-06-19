#ifndef NOVA_IPC_H
#define NOVA_IPC_H

#include <stdint.h>

#define NOVA_IPC_MAX_MAILBOXES  16
#define NOVA_IPC_MAX_MESSAGES    8
#define NOVA_IPC_MAX_PAYLOAD   128
#define NOVA_PIPE_MAX            8
#define NOVA_PIPE_BUFFER       256
#define NOVA_SHM_MAX            8
#define NOVA_SHM_NAME_MAX      32
#define NOVA_SHM_DATA_MAX      512

typedef struct {
    int valid;
    int src_pid;
    int dst_pid;
    uint32_t timestamp_ms;
    uint32_t length;
    char payload[NOVA_IPC_MAX_PAYLOAD];
} nova_ipc_message_t;

typedef struct {
    int valid;
    int pid;
    char name[32];
    uint32_t sent_count;
    uint32_t recv_count;
    uint32_t dropped_count;
    nova_ipc_message_t queue[NOVA_IPC_MAX_MESSAGES];
} nova_mailbox_t;

typedef struct {
    int valid;
    int id;
    int owner_pid;
    char name[32];
    uint8_t buffer[NOVA_PIPE_BUFFER];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t used;
    uint32_t total_written;
    uint32_t total_read;
} nova_pipe_t;

typedef struct {
    int valid;
    int id;
    int owner_pid;
    char name[NOVA_SHM_NAME_MAX];
    uint32_t size;
    uint32_t refs;
    uint8_t data[NOVA_SHM_DATA_MAX];
} nova_shm_region_t;

void ipc_init(void);
int  ipc_register_mailbox(int pid, const char *name);
int  ipc_send(int src_pid, int dst_pid, const char *payload, uint32_t len);
int  ipc_recv(int dst_pid, char *out, uint32_t max, int *out_src_pid);
int  ipc_pipe_create(int owner_pid, const char *name);
int  ipc_pipe_write(int pipe_id, const char *data, uint32_t len);
int  ipc_pipe_read(int pipe_id, char *out, uint32_t max);
int  ipc_shm_create(int owner_pid, const char *name, uint32_t size);
int  ipc_shm_write(int shm_id, const char *data, uint32_t len);
int  ipc_shm_read(int shm_id, char *out, uint32_t max);
void ipc_mailboxes_report(char *buf, int max);
void ipc_pipes_report(char *buf, int max);
void ipc_shm_report(char *buf, int max);
void ipc_publish_runtime(void);

#endif
