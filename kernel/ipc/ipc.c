#include "ipc.h"
#include "../sched/scheduler.h"
#include "../timer.h"
#include "../../fs/vfs.h"
#include "../../libc.h"
#include <stddef.h>

static nova_mailbox_t g_mailboxes[NOVA_IPC_MAX_MAILBOXES];
static nova_pipe_t g_pipes[NOVA_PIPE_MAX];
static nova_shm_region_t g_shm[NOVA_SHM_MAX];

static int ipc_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void ipc_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void ipc_cat(char *dst, const char *src, int max) {
    int dl = ipc_len(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void ipc_itoa(uint32_t value, char *buf, int max) {
    char tmp[16];
    int pos = 0;
    int out = 0;
    if (!buf || max <= 0) return;
    if (!value) {
        buf[0] = '0';
        if (max > 1) buf[1] = 0;
        return;
    }
    while (value && pos < (int)sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}

static nova_mailbox_t *ipc_find_mailbox(int pid) {
    for (int i = 0; i < NOVA_IPC_MAX_MAILBOXES; ++i) if (g_mailboxes[i].valid && g_mailboxes[i].pid == pid) return &g_mailboxes[i];
    return NULL;
}

static nova_pipe_t *ipc_find_pipe(int id) {
    for (int i = 0; i < NOVA_PIPE_MAX; ++i) if (g_pipes[i].valid && g_pipes[i].id == id) return &g_pipes[i];
    return NULL;
}

static nova_shm_region_t *ipc_find_shm(int id) {
    for (int i = 0; i < NOVA_SHM_MAX; ++i) if (g_shm[i].valid && g_shm[i].id == id) return &g_shm[i];
    return NULL;
}

void ipc_init(void) {
    k_memset(g_mailboxes, 0, sizeof(g_mailboxes));
    k_memset(g_pipes, 0, sizeof(g_pipes));
    k_memset(g_shm, 0, sizeof(g_shm));
}

int ipc_register_mailbox(int pid, const char *name) {
    nova_mailbox_t *mb = ipc_find_mailbox(pid);
    if (mb) {
        ipc_copy(mb->name, name, sizeof(mb->name));
        return 1;
    }
    for (int i = 0; i < NOVA_IPC_MAX_MAILBOXES; ++i) {
        if (!g_mailboxes[i].valid) {
            g_mailboxes[i].valid = 1;
            g_mailboxes[i].pid = pid;
            ipc_copy(g_mailboxes[i].name, name, sizeof(g_mailboxes[i].name));
            return 1;
        }
    }
    return 0;
}

int ipc_send(int src_pid, int dst_pid, const char *payload, uint32_t len) {
    nova_mailbox_t *mb = ipc_find_mailbox(dst_pid);
    if (!mb || !payload) return -1;
    for (int i = 0; i < NOVA_IPC_MAX_MESSAGES; ++i) {
        if (!mb->queue[i].valid) {
            uint32_t copy_len = len;
            if (copy_len >= NOVA_IPC_MAX_PAYLOAD) copy_len = NOVA_IPC_MAX_PAYLOAD - 1;
            mb->queue[i].valid = 1;
            mb->queue[i].src_pid = src_pid;
            mb->queue[i].dst_pid = dst_pid;
            mb->queue[i].timestamp_ms = timer_ms();
            mb->queue[i].length = copy_len;
            k_memcpy(mb->queue[i].payload, payload, copy_len);
            mb->queue[i].payload[copy_len] = 0;
            mb->sent_count++;
            scheduler_account_ipc(src_pid, copy_len, 0);
            scheduler_account_ipc(dst_pid, copy_len, 0);
            return (int)copy_len;
        }
    }
    mb->dropped_count++;
    return -1;
}

int ipc_recv(int dst_pid, char *out, uint32_t max, int *out_src_pid) {
    nova_mailbox_t *mb = ipc_find_mailbox(dst_pid);
    if (!mb || !out || max == 0) return -1;
    for (int i = 0; i < NOVA_IPC_MAX_MESSAGES; ++i) {
        if (mb->queue[i].valid) {
            uint32_t copy_len = mb->queue[i].length;
            if (copy_len >= max) copy_len = max - 1;
            k_memcpy(out, mb->queue[i].payload, copy_len);
            out[copy_len] = 0;
            if (out_src_pid) *out_src_pid = mb->queue[i].src_pid;
            mb->queue[i].valid = 0;
            mb->recv_count++;
            return (int)copy_len;
        }
    }
    out[0] = 0;
    return 0;
}

int ipc_pipe_create(int owner_pid, const char *name) {
    for (int i = 0; i < NOVA_PIPE_MAX; ++i) {
        if (!g_pipes[i].valid) {
            g_pipes[i].valid = 1;
            g_pipes[i].id = i + 1;
            g_pipes[i].owner_pid = owner_pid;
            ipc_copy(g_pipes[i].name, name && name[0] ? name : "pipe", sizeof(g_pipes[i].name));
            return g_pipes[i].id;
        }
    }
    return -1;
}

int ipc_pipe_write(int pipe_id, const char *data, uint32_t len) {
    nova_pipe_t *pipe = ipc_find_pipe(pipe_id);
    uint32_t written = 0;
    if (!pipe || !data) return -1;
    while (written < len && pipe->used < NOVA_PIPE_BUFFER) {
        pipe->buffer[pipe->write_pos] = (uint8_t)data[written];
        pipe->write_pos = (pipe->write_pos + 1u) % NOVA_PIPE_BUFFER;
        pipe->used++;
        written++;
    }
    pipe->total_written += written;
    scheduler_account_ipc(pipe->owner_pid, written, 0);
    return (int)written;
}

int ipc_pipe_read(int pipe_id, char *out, uint32_t max) {
    nova_pipe_t *pipe = ipc_find_pipe(pipe_id);
    uint32_t read = 0;
    if (!pipe || !out || max == 0) return -1;
    while (read + 1u < max && pipe->used > 0) {
        out[read++] = (char)pipe->buffer[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1u) % NOVA_PIPE_BUFFER;
        pipe->used--;
    }
    out[read] = 0;
    pipe->total_read += read;
    scheduler_account_ipc(pipe->owner_pid, read, 0);
    return (int)read;
}

int ipc_shm_create(int owner_pid, const char *name, uint32_t size) {
    for (int i = 0; i < NOVA_SHM_MAX; ++i) {
        if (g_shm[i].valid && name && !k_strcmp(g_shm[i].name, name)) {
            g_shm[i].refs++;
            return g_shm[i].id;
        }
    }
    for (int i = 0; i < NOVA_SHM_MAX; ++i) {
        if (!g_shm[i].valid) {
            g_shm[i].valid = 1;
            g_shm[i].id = i + 1;
            g_shm[i].owner_pid = owner_pid;
            g_shm[i].size = size > NOVA_SHM_DATA_MAX ? NOVA_SHM_DATA_MAX : size;
            g_shm[i].refs = 1;
            ipc_copy(g_shm[i].name, name && name[0] ? name : "shm", sizeof(g_shm[i].name));
            scheduler_account_ipc(owner_pid, 0, g_shm[i].size);
            return g_shm[i].id;
        }
    }
    return -1;
}

int ipc_shm_write(int shm_id, const char *data, uint32_t len) {
    nova_shm_region_t *region = ipc_find_shm(shm_id);
    uint32_t copy_len;
    if (!region || !data) return -1;
    copy_len = len > region->size ? region->size : len;
    k_memcpy(region->data, data, copy_len);
    if (copy_len < region->size) region->data[copy_len] = 0;
    scheduler_account_ipc(region->owner_pid, 0, copy_len);
    return (int)copy_len;
}

int ipc_shm_read(int shm_id, char *out, uint32_t max) {
    nova_shm_region_t *region = ipc_find_shm(shm_id);
    uint32_t copy_len;
    if (!region || !out || max == 0) return -1;
    copy_len = region->size;
    if (copy_len >= max) copy_len = max - 1;
    k_memcpy(out, region->data, copy_len);
    out[copy_len] = 0;
    scheduler_account_ipc(region->owner_pid, 0, copy_len);
    return (int)copy_len;
}

void ipc_mailboxes_report(char *buf, int max) {
    char nb[16];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    ipc_cat(buf, "Nova IPC Mailboxes\n", max);
    ipc_cat(buf, "PID NAME SENT RECV DROP\n", max);
    ipc_cat(buf, "-----------------------\n", max);
    for (int i = 0; i < NOVA_IPC_MAX_MAILBOXES; ++i) {
        if (!g_mailboxes[i].valid) continue;
        ipc_itoa((uint32_t)g_mailboxes[i].pid, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_cat(buf, g_mailboxes[i].name, max); ipc_cat(buf, " ", max);
        ipc_itoa(g_mailboxes[i].sent_count, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_itoa(g_mailboxes[i].recv_count, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_itoa(g_mailboxes[i].dropped_count, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, "\n", max);
    }
}

void ipc_pipes_report(char *buf, int max) {
    char nb[16];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    ipc_cat(buf, "Nova Pipes\n", max);
    ipc_cat(buf, "ID OWNER USED NAME\n", max);
    ipc_cat(buf, "------------------\n", max);
    for (int i = 0; i < NOVA_PIPE_MAX; ++i) {
        if (!g_pipes[i].valid) continue;
        ipc_itoa((uint32_t)g_pipes[i].id, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_itoa((uint32_t)g_pipes[i].owner_pid, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_itoa(g_pipes[i].used, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_cat(buf, g_pipes[i].name, max); ipc_cat(buf, "\n", max);
    }
}

void ipc_shm_report(char *buf, int max) {
    char nb[16];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    ipc_cat(buf, "Nova Shared Memory\n", max);
    ipc_cat(buf, "ID OWNER SIZE REFS NAME\n", max);
    ipc_cat(buf, "-----------------------\n", max);
    for (int i = 0; i < NOVA_SHM_MAX; ++i) {
        if (!g_shm[i].valid) continue;
        ipc_itoa((uint32_t)g_shm[i].id, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_itoa((uint32_t)g_shm[i].owner_pid, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_itoa(g_shm[i].size, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_itoa(g_shm[i].refs, nb, sizeof(nb)); ipc_cat(buf, nb, max); ipc_cat(buf, " ", max);
        ipc_cat(buf, g_shm[i].name, max); ipc_cat(buf, "\n", max);
    }
}

void ipc_publish_runtime(void) {
    char mail[2048];
    char pipes[1024];
    char shm[1024];
    (void)vfs_mkdir("/var");
    (void)vfs_mkdir("/var/run");
    (void)vfs_mkdir("/var/run/ipc");
    (void)vfs_mkdir("/proc");
    ipc_mailboxes_report(mail, sizeof(mail));
    ipc_pipes_report(pipes, sizeof(pipes));
    ipc_shm_report(shm, sizeof(shm));
    (void)vfs_write_file("/proc/ipc", mail, (uint32_t)ipc_len(mail));
    (void)vfs_write_file("/proc/pipes", pipes, (uint32_t)ipc_len(pipes));
    (void)vfs_write_file("/proc/shm", shm, (uint32_t)ipc_len(shm));
    (void)vfs_write_file("/var/run/ipc/mailboxes.txt", mail, (uint32_t)ipc_len(mail));
    (void)vfs_write_file("/var/run/ipc/pipes.txt", pipes, (uint32_t)ipc_len(pipes));
    (void)vfs_write_file("/var/run/ipc/shm.txt", shm, (uint32_t)ipc_len(shm));
}
