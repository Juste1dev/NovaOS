#include "userspace.h"
#include "timer.h"
#include "syscalls.h"
#include "sched/scheduler.h"
#include "ipc/ipc.h"
#include "elf/elf.h"
#include "users.h"
#include "../fs/vfs.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

userspace_state_t g_userspace;
static int g_userspace_seeded = 0;

static int us_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void us_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void us_cat(char *dst, const char *src, int max) {
    int dl = us_len(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void us_itoa(int value, char *buf, int max) {
    char tmp[16];
    int pos = 0;
    int out = 0;
    unsigned int n;
    if (!buf || max <= 0) return;
    if (value == 0) {
        buf[0] = '0';
        if (max > 1) buf[1] = 0;
        return;
    }
    if (value < 0) {
        if (out < max - 1) buf[out++] = '-';
        n = (unsigned int)(-value);
    } else {
        n = (unsigned int)value;
    }
    while (n > 0 && pos < (int)sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (n % 10u));
        n /= 10u;
    }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}

static const char *us_ring(int ring) {
    return ring == NOVA_RING_USER ? "ring3" : "ring0";
}

static void us_append_line(char *buf, int max, const char *line) {
    us_cat(buf, line, max);
    us_cat(buf, "\n", max);
}

static void userspace_register(const char *name, const char *role, const char *state, const char *endpoint, int privileged, int priority, int session_id, const char *format) {
    userspace_process_t *p;
    int pid;
    int tid;
    int flags = privileged ? (NOVA_PROC_KERNEL | NOVA_PROC_SERVICE) : NOVA_PROC_USER;
    if (g_userspace.process_count >= USERSPACE_MAX_PROCS) return;
    pid = scheduler_create_process(name, endpoint, priority, flags);
    if (pid < 0) return;
    tid = scheduler_create_thread(pid, name, priority, privileged);
    if (tid < 0) return;
    scheduler_set_process_security(pid, privileged ? 0 : 1000, privileged ? 0 : 1000, session_id, privileged ? NOVA_RING_KERNEL : NOVA_RING_USER, privileged ? 0 : 1);
    scheduler_set_process_image(pid, endpoint, 0x0000000000400000ULL + ((uint64_t)pid * 0x10000ULL), 0x00007FFF00000000ULL - ((uint64_t)pid * 0x200000ULL));
    (void)ipc_register_mailbox(pid, name);
    p = &g_userspace.processes[g_userspace.process_count];
    k_memset(p, 0, sizeof(*p));
    p->pid = pid;
    p->tid = tid;
    p->uid = privileged ? 0 : 1000;
    p->gid = privileged ? 0 : 1000;
    p->session_id = session_id;
    p->ring = privileged ? NOVA_RING_KERNEL : NOVA_RING_USER;
    p->isolated = privileged ? 0 : 1;
    us_copy(p->name, name, sizeof(p->name));
    us_copy(p->role, role, sizeof(p->role));
    us_copy(p->state, state, sizeof(p->state));
    us_copy(p->endpoint, endpoint, sizeof(p->endpoint));
    us_copy(p->exec_format, format, sizeof(p->exec_format));
    us_copy(p->mailbox, name, sizeof(p->mailbox));
    p->started_ms = timer_ms();
    p->privileged = privileged;
    p->priority = priority;
    g_userspace.process_count++;
}

int userspace_process_count(void) {
    return g_userspace.process_count;
}

void userspace_report(char *buf, int max) {
    char nb[16];
    const user_session_t *session = users_current_session();
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    us_append_line(buf, max, "Userspace Nova 6.3 actif");
    us_append_line(buf, max, "Mode: userspace isolé logique + scheduler préemptif + API syscalls étendue");
    us_append_line(buf, max, "Fonctions: IPC, pipes, shared memory, fork/exec, signaux et loader ELF64/script");
    us_append_line(buf, max, "Rings: ring0 pour services noyau, ring3 prêt pour les binaires utilisateur");
    us_append_line(buf, max, "Racines: /usr  /usr/bin  /usr/share/userspace  /var/run/userspace  /proc  /var/run/ipc");
    us_cat(buf, "Processus actifs: ", max);
    us_itoa(g_userspace.process_count, nb, sizeof(nb));
    us_cat(buf, nb, max);
    us_cat(buf, "\nThreads actifs: ", max);
    us_itoa(scheduler_thread_count(), nb, sizeof(nb));
    us_cat(buf, nb, max);
    us_cat(buf, "\nSession courante: ", max);
    if (session) {
        us_cat(buf, session->username, max);
        us_cat(buf, " (sid=", max);
        us_itoa(session->session_id, nb, sizeof(nb));
        us_cat(buf, nb, max);
        us_cat(buf, ")", max);
    } else {
        us_cat(buf, "bootstrap", max);
    }
    us_cat(buf, "\n", max);
}

void userspace_process_table(char *buf, int max) {
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    us_append_line(buf, max, "PID TID UID SID RING ISO PRI ROLE FORMAT NAME");
    us_append_line(buf, max, "--------------------------------------------------------------");
    for (int i = 0; i < g_userspace.process_count; i++) {
        char line[192];
        char nb[16];
        userspace_process_t *p = &g_userspace.processes[i];
        k_memset(line, 0, sizeof(line));
        us_itoa(p->pid, nb, sizeof(nb)); us_cat(line, nb, sizeof(line)); us_cat(line, " ", sizeof(line));
        us_itoa(p->tid, nb, sizeof(nb)); us_cat(line, nb, sizeof(line)); us_cat(line, " ", sizeof(line));
        us_itoa(p->uid, nb, sizeof(nb)); us_cat(line, nb, sizeof(line)); us_cat(line, " ", sizeof(line));
        us_itoa(p->session_id, nb, sizeof(nb)); us_cat(line, nb, sizeof(line)); us_cat(line, " ", sizeof(line));
        us_cat(line, us_ring(p->ring), sizeof(line)); us_cat(line, " ", sizeof(line));
        us_cat(line, p->isolated ? "yes" : "no", sizeof(line)); us_cat(line, " ", sizeof(line));
        us_itoa(p->priority, nb, sizeof(nb)); us_cat(line, nb, sizeof(line)); us_cat(line, " ", sizeof(line));
        us_cat(line, p->role, sizeof(line)); us_cat(line, " ", sizeof(line));
        us_cat(line, p->exec_format, sizeof(line)); us_cat(line, " ", sizeof(line));
        us_cat(line, p->name, sizeof(line));
        us_append_line(buf, max, line);
    }
}

void userspace_init(void) {
    k_memset(&g_userspace, 0, sizeof(g_userspace));
    g_userspace.boot_tick_ms = timer_ms();
    userspace_register("initd", "bootstrap", "running", "/usr/bin/initd", 1, NOVA_PRIORITY_HIGH, 0, "script");
    userspace_register("sessiond", "session", "running", "/usr/bin/sessiond", 1, NOVA_PRIORITY_HIGH, 0, "script");
    userspace_register("ipcd", "ipc", "running", "/usr/bin/ipcd", 1, NOVA_PRIORITY_HIGH, 0, "script");
    userspace_register("shmd", "shm", "running", "/usr/bin/shmd", 1, NOVA_PRIORITY_HIGH, 0, "script");
    userspace_register("shell", "terminal", "ready", "/usr/bin/shell", 0, NOVA_PRIORITY_NORMAL, 1, "script");
    userspace_register("nova-shell", "shell", "ready", "/usr/bin/nova-shell", 0, NOVA_PRIORITY_NORMAL, 1, "script");
    userspace_register("fileman", "files", "ready", "/usr/bin/fileman", 0, NOVA_PRIORITY_NORMAL, 1, "script");
    userspace_register("symerad", "assistant", "ready", "/usr/bin/symerad", 0, NOVA_PRIORITY_NORMAL, 1, "script");
    userspace_register("browserd", "browser", "ready", "/usr/bin/browserd", 0, NOVA_PRIORITY_LOW, 1, "script");
    g_userspace.ready = 1;
}

void userspace_publish_vfs(void) {
    char report[1024];
    char table[1536];
    char sched[2048];
    char threads[2048];
    char syscalls[2048];
    char ipc_report[2048];
    char pipes[1024];
    char shm[1024];
    char elf_report[2048];
    static const char *readme =
        "Nova Userspace\n"
        "==============\n"
        "\n"
        "Userspace enrichi avec isolation logique, IPC, pipes, mémoire partagée, fork/exec et signaux.\n"
        "Procfs : /proc/processes  /proc/scheduler  /proc/syscalls  /proc/ipc  /proc/pipes  /proc/shm  /proc/elf\n";
    static const char *shell_doc =
        "Nova Shell\n"
        "==========\n"
        "Binaire de session : /usr/bin/nova-shell\n"
        "API : fork/exec/signal, pipes et shared memory exposés par syscalls.\n";

    if (!g_userspace.ready) userspace_init();

    (void)vfs_mkdir("/usr");
    (void)vfs_mkdir("/usr/bin");
    (void)vfs_mkdir("/usr/share");
    (void)vfs_mkdir("/usr/share/userspace");
    (void)vfs_mkdir("/var");
    (void)vfs_mkdir("/var/log");
    (void)vfs_mkdir("/var/run");
    (void)vfs_mkdir("/var/run/userspace");
    (void)vfs_mkdir("/proc");
    (void)vfs_mkdir("/etc");

    (void)vfs_write_file("/usr/bin/initd", "#!/nova/initd\nservice=bootstrap\n", 31);
    (void)vfs_write_file("/usr/bin/sessiond", "#!/nova/sessiond\nservice=session\n", 33);
    (void)vfs_write_file("/usr/bin/ipcd", "#!/nova/ipcd\nservice=ipc\n", 26);
    (void)vfs_write_file("/usr/bin/shmd", "#!/nova/shmd\nservice=shm\n", 26);
    (void)vfs_write_file("/usr/bin/shell", "#!/nova/shell\nentry=terminal\n", 30);
    (void)vfs_write_file("/usr/bin/nova-shell", "#!/nova/shell\nentry=system-shell\n", 34);
    (void)vfs_write_file("/usr/bin/fileman", "#!/nova/fileman\nentry=files\n", 30);
    (void)vfs_write_file("/usr/bin/symerad", "#!/nova/symerad\nentry=assistant\n", 34);
    (void)vfs_write_file("/usr/bin/browserd", "#!/nova/browserd\nentry=browser\n", 32);

    if (!g_userspace_seeded) {
        int shell_pid = g_userspace.processes[4].pid;
        int init_pid = g_userspace.processes[0].pid;
        int pipe_id = ipc_pipe_create(shell_pid, "shell.log");
        int shm_id = ipc_shm_create(g_userspace.processes[3].pid, "desktop.theme", 64);
        (void)ipc_pipe_write(pipe_id, "Nova shell ready\n", 17);
        (void)ipc_shm_write(shm_id, "accent=azure;mode=dark", 24);
        (void)ipc_send(init_pid, shell_pid, "boot-ready", 10);
        g_userspace_seeded = 1;
    }

    elf_publish_demo();
    userspace_report(report, sizeof(report));
    userspace_process_table(table, sizeof(table));
    scheduler_snapshot(sched, sizeof(sched));
    scheduler_threads_snapshot(threads, sizeof(threads));
    syscall_describe(syscalls, sizeof(syscalls));
    ipc_mailboxes_report(ipc_report, sizeof(ipc_report));
    ipc_pipes_report(pipes, sizeof(pipes));
    ipc_shm_report(shm, sizeof(shm));
    elf_runtime_report(elf_report, sizeof(elf_report));

    (void)vfs_write_file("/usr/share/userspace/README.txt", readme, (uint32_t)us_len(readme));
    (void)vfs_write_file("/usr/share/userspace/shell.txt", shell_doc, (uint32_t)us_len(shell_doc));
    (void)vfs_write_file("/usr/share/userspace/report.txt", report, (uint32_t)us_len(report));
    (void)vfs_write_file("/var/run/userspace/processes.txt", table, (uint32_t)us_len(table));
    (void)vfs_write_file("/var/log/userspace.log", table, (uint32_t)us_len(table));
    (void)vfs_write_file("/home/user/Documents/Userspace.txt", report, (uint32_t)us_len(report));
    (void)vfs_write_file("/proc/processes", table, (uint32_t)us_len(table));
    (void)vfs_write_file("/proc/scheduler", sched, (uint32_t)us_len(sched));
    (void)vfs_write_file("/proc/threads", threads, (uint32_t)us_len(threads));
    (void)vfs_write_file("/proc/syscalls", syscalls, (uint32_t)us_len(syscalls));
    (void)vfs_write_file("/proc/ipc", ipc_report, (uint32_t)us_len(ipc_report));
    (void)vfs_write_file("/proc/pipes", pipes, (uint32_t)us_len(pipes));
    (void)vfs_write_file("/proc/shm", shm, (uint32_t)us_len(shm));
    (void)vfs_write_file("/proc/elf", elf_report, (uint32_t)us_len(elf_report));
    ipc_publish_runtime();
}
