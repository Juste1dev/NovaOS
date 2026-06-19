#include "shell_ext.h"
#include "../fs/vfs.h"
#include "../kernel/users.h"
#include "../kernel/ssh.h"
#include "../kernel/module_loader.h"
#include "../kernel/timer.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

#define SH_MAX_CMD   512
#define SH_MAX_ARGS  24
#define SH_MAX_OUT   8192
#define SH_MAX_JOBS  8

typedef struct {
    int id;
    int active;
    int done;
    uint32_t done_at;
    char command[96];
} shell_job_t;

static shell_job_t g_jobs[SH_MAX_JOBS];
static int g_next_job_id = 1;

static int sh_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void sh_copy(char *d, const char *s, int m) { int i = 0; if (!d || m <= 0) return; while (s && s[i] && i < m - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static void sh_cat(char *d, const char *s, int m) { int i = sh_len(d), j = 0; if (!d || !s || m <= 0 || i >= m - 1) return; while (s[j] && i + j < m - 1) { d[i + j] = s[j]; j++; } d[i + j] = 0; }
static int sh_eq(const char *a, const char *b) { return k_strcmp(a ? a : "", b ? b : "") == 0; }
static int sh_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int sh_is_num(const char *s) { int i = 0; if (!s || !s[0]) return 0; for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0; return 1; }
static int sh_atoi(const char *s) { int n = 0; if (!s) return 0; while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; } return n; }
static void sh_itoa(int value, char *buf, int max) {
    char tmp[16]; int pos = 0, out = 0; unsigned int n = (unsigned int)(value < 0 ? -value : value);
    if (!buf || max <= 0) return; if (value < 0 && out < max - 1) buf[out++] = '-';
    if (n == 0) { buf[out++] = '0'; buf[out] = 0; return; }
    while (n > 0 && pos < (int)sizeof(tmp)) { tmp[pos++] = (char)('0' + (n % 10u)); n /= 10u; }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos]; buf[out] = 0;
}
static char sh_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int sh_ieq(const char *a, const char *b) {
    while (*a && *b) { if (sh_lower(*a) != sh_lower(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}
static void sh_trim(char *s) {
    int len = sh_len(s), start = 0;
    while (start < len && sh_space(s[start])) start++;
    while (len > start && sh_space(s[len - 1])) len--;
    if (start > 0) k_memmove(s, s + start, (size_t)(len - start));
    s[len - start] = 0;
}
static int sh_has_glob(const char *s) { for (int i = 0; s && s[i]; i++) if (s[i] == '*' || s[i] == '?') return 1; return 0; }
static int sh_match(const char *text, const char *pat) {
    if (!*pat) return !*text;
    if (*pat == '*') return sh_match(text, pat + 1) || (*text && sh_match(text + 1, pat));
    if (*pat == '?') return *text && sh_match(text + 1, pat + 1);
    return (*text == *pat) && sh_match(text + 1, pat + 1);
}
static void sh_parse_args(char *cmd, char **argv, int *argc) {
    int i = 0, in_token = 0; *argc = 0;
    while (cmd[i] && *argc < SH_MAX_ARGS - 1) {
        if (sh_space(cmd[i])) { if (in_token) { cmd[i] = 0; in_token = 0; } }
        else { if (!in_token) { argv[(*argc)++] = &cmd[i]; in_token = 1; } }
        i++;
    }
    argv[*argc] = NULL;
}
static void sh_join_args(char **argv, int start, int argc, char *out, int max) {
    out[0] = 0;
    for (int i = start; i < argc; i++) { if (i > start) sh_cat(out, " ", max); sh_cat(out, argv[i], max); }
}
static void sh_render(void *ctx, const shell_ext_api_t *api, const char *text) {
    char buf[SH_MAX_OUT];
    int start = 0, len;
    if (!api || !api->println) return;
    if (!text || !text[0]) return;
    sh_copy(buf, text, sizeof(buf));
    len = sh_len(buf);
    for (int i = 0; i <= len; i++) {
        if (buf[i] == '\n' || buf[i] == 0) {
            char save = buf[i];
            buf[i] = 0;
            api->println(ctx, &buf[start]);
            buf[i] = save;
            start = i + 1;
        }
    }
}
static void sh_update_jobs(void) {
    uint32_t now = timer_ms();
    for (int i = 0; i < SH_MAX_JOBS; i++) if (g_jobs[i].active && !g_jobs[i].done && now >= g_jobs[i].done_at) g_jobs[i].done = 1;
}
static int sh_add_job(const char *command, uint32_t delay_ms) {
    for (int i = 0; i < SH_MAX_JOBS; i++) {
        if (!g_jobs[i].active) {
            g_jobs[i].active = 1;
            g_jobs[i].done = 0;
            g_jobs[i].id = g_next_job_id++;
            g_jobs[i].done_at = timer_ms() + delay_ms;
            sh_copy(g_jobs[i].command, command, sizeof(g_jobs[i].command));
            return g_jobs[i].id;
        }
    }
    return -1;
}
static shell_job_t *sh_find_job(int id) {
    for (int i = 0; i < SH_MAX_JOBS; i++) if (g_jobs[i].active && g_jobs[i].id == id) return &g_jobs[i];
    return NULL;
}
static void sh_glob_expand(void *ctx, const shell_ext_api_t *api, const char *pattern, char matches[][256], int *count, int max) {
    char abs[256];
    *count = 0;
    if (!api || !api->resolve_path) return;
    api->resolve_path(ctx, pattern, abs, sizeof(abs));
    for (int i = 0; i < vfs_node_count && *count < max; i++) {
        if (!vfs_nodes[i].valid) continue;
        if (sh_match(vfs_nodes[i].path, abs) || sh_match(vfs_nodes[i].name, pattern)) {
            sh_copy(matches[*count], vfs_nodes[i].path, 256);
            (*count)++;
        }
    }
}
static void sh_slurp(const char *path, char *buf, int max) { if (!buf || max <= 0) return; k_memset(buf, 0, (size_t)max); vfs_get_contents(path, buf, (uint32_t)(max - 1)); }
static void sh_append_mode_owner(const char *path, char *out, int max) {
    char mode[16], nb[16]; vfs_node_t *n = vfs_get_node(path);
    if (!n) return;
    vfs_format_mode(path, mode, sizeof(mode));
    sh_cat(out, mode, max); sh_cat(out, " ", max);
    sh_itoa((int)n->owner_uid, nb, sizeof(nb)); sh_cat(out, nb, max); sh_cat(out, ":", max);
    sh_itoa((int)n->owner_gid, nb, sizeof(nb)); sh_cat(out, nb, max);
}
static void sh_wc_text(const char *text, char *out, int max) {
    int lines = 0, words = 0, bytes = sh_len(text), in_word = 0;
    char nb[16];
    out[0] = 0;
    for (int i = 0; text && text[i]; i++) {
        if (text[i] == '\n') lines++;
        if (sh_space(text[i])) in_word = 0; else if (!in_word) { in_word = 1; words++; }
    }
    if (bytes > 0 && text[bytes - 1] != '\n') lines++;
    sh_itoa(lines, nb, sizeof(nb)); sh_cat(out, nb, max); sh_cat(out, " lines ", max);
    sh_itoa(words, nb, sizeof(nb)); sh_cat(out, nb, max); sh_cat(out, " words ", max);
    sh_itoa(bytes, nb, sizeof(nb)); sh_cat(out, nb, max); sh_cat(out, " bytes\n", max);
}
static void sh_head_tail(const char *text, int n, int tail, char *out, int max) {
    int total = 0, start_line = 0, cur = 0, begin = 0, len = sh_len(text);
    out[0] = 0; if (n <= 0) n = 10;
    for (int i = 0; i < len; i++) if (text[i] == '\n') total++;
    if (len > 0 && text[len - 1] != '\n') total++;
    if (tail && total > n) start_line = total - n;
    for (int i = 0; i <= len; i++) {
        if (text[i] == '\n' || text[i] == 0) {
            if ((!tail && cur < n) || (tail && cur >= start_line)) {
                char line[512]; int l = i - begin; if (l >= (int)sizeof(line)) l = (int)sizeof(line) - 1;
                k_memset(line, 0, sizeof(line)); if (l > 0) k_memcpy(line, text + begin, (size_t)l);
                sh_cat(out, line, max); sh_cat(out, "\n", max);
            }
            cur++; begin = i + 1; if (!tail && cur >= n) break;
        }
    }
}
static int sh_collect_simple(void *ctx, const shell_ext_api_t *api, const char *cmd_line, const char *stdin_text, char *out, int max) {
    char cmd[SH_MAX_CMD]; char *argv[SH_MAX_ARGS]; int argc = 0; char joined[512];
    char path[256]; char buf[SH_MAX_OUT];
    out[0] = 0;
    sh_copy(cmd, cmd_line, sizeof(cmd)); sh_trim(cmd); if (!cmd[0]) return 0;
    sh_parse_args(cmd, argv, &argc); if (argc == 0) return 0;

    if (sh_eq(argv[0], "echo")) { sh_join_args(argv, 1, argc, out, max); sh_cat(out, "\n", max); return 1; }
    if (sh_eq(argv[0], "pwd")) { sh_cat(out, api->get_cwd ? api->get_cwd(ctx) : "/", max); sh_cat(out, "\n", max); return 1; }
    if (sh_eq(argv[0], "whoami")) { sh_cat(out, api->get_username ? api->get_username(ctx) : "user", max); sh_cat(out, "\n", max); return 1; }
    if (sh_eq(argv[0], "hostname")) { sh_slurp("/etc/hostname", out, max); return 1; }
    if (sh_eq(argv[0], "id")) {
        user_t *u = users_get_current(); char groups[128], nb[16]; groups[0] = 0;
        users_groups_for(u ? u->username : "user", groups, sizeof(groups));
        sh_cat(out, "uid=", max); sh_itoa(u ? u->uid : 1000, nb, sizeof(nb)); sh_cat(out, nb, max);
        sh_cat(out, "(", max); sh_cat(out, u ? u->username : "user", max); sh_cat(out, ") gid=", max);
        sh_itoa(u ? u->gid : 1000, nb, sizeof(nb)); sh_cat(out, nb, max); sh_cat(out, " groups=", max); sh_cat(out, groups, max); sh_cat(out, "\n", max); return 1;
    }
    if (sh_eq(argv[0], "groups")) {
        char groups[128]; groups[0] = 0; users_groups_for(api->get_username ? api->get_username(ctx) : "user", groups, sizeof(groups)); sh_cat(out, groups, max); sh_cat(out, "\n", max); return 1;
    }
    if (sh_eq(argv[0], "features")) { sh_slurp("/home/user/Documents/Documentation-Technique.txt", out, max); return 1; }
    if (sh_eq(argv[0], "syscalls")) { sh_slurp("/proc/syscalls", out, max); return 1; }
    if (sh_eq(argv[0], "ipc")) { sh_slurp("/proc/ipc", out, max); return 1; }
    if (sh_eq(argv[0], "pipes")) { sh_slurp("/proc/pipes", out, max); return 1; }
    if (sh_eq(argv[0], "shm")) { sh_slurp("/proc/shm", out, max); return 1; }
    if (sh_eq(argv[0], "elf")) { sh_slurp("/proc/elf", out, max); return 1; }
    if (sh_eq(argv[0], "drivers")) { sh_slurp("/proc/drivers", out, max); return 1; }
    if (sh_eq(argv[0], "lspci") || sh_eq(argv[0], "pci")) { sh_slurp("/proc/pci", out, max); return 1; }
    if (sh_eq(argv[0], "lsdev")) { sh_slurp("/proc/devices", out, max); return 1; }
    if (sh_eq(argv[0], "sessions")) { sh_slurp("/var/run/sessions/list", out, max); return 1; }
    if (sh_eq(argv[0], "logs")) { sh_slurp("/var/log/system.log", out, max); return 1; }
    if (sh_eq(argv[0], "journal")) { sh_slurp("/var/log/fs-journal.log", out, max); return 1; }
    if (sh_eq(argv[0], "klog")) { sh_slurp("/var/log/kernel.log", out, max); return 1; }
    if (sh_eq(argv[0], "lsmod")) { sh_slurp("/proc/modules", out, max); return 1; }
    if (sh_eq(argv[0], "sshd") && argc > 1 && sh_eq(argv[1], "status")) { ssh_status_report(out, max); return 1; }

    if (sh_eq(argv[0], "cat")) {
        if (argc < 2) { if (stdin_text) sh_cat(out, stdin_text, max); return 1; }
        for (int a = 1; a < argc; a++) {
            char matches[16][256]; int count = 0;
            if (sh_has_glob(argv[a])) sh_glob_expand(ctx, api, argv[a], matches, &count, 16);
            if (count == 0) { api->resolve_path(ctx, argv[a], path, sizeof(path)); sh_slurp(path, buf, sizeof(buf)); sh_cat(out, buf, max); }
            else for (int m = 0; m < count; m++) { sh_slurp(matches[m], buf, sizeof(buf)); sh_cat(out, buf, max); if (m + 1 < count) sh_cat(out, "\n", max); }
        }
        if (sh_len(out) > 0 && out[sh_len(out) - 1] != '\n') sh_cat(out, "\n", max);
        return 1;
    }
    if (sh_eq(argv[0], "wc")) {
        if (argc < 2) sh_wc_text(stdin_text ? stdin_text : "", out, max);
        else { api->resolve_path(ctx, argv[1], path, sizeof(path)); sh_slurp(path, buf, sizeof(buf)); sh_wc_text(buf, out, max); }
        return 1;
    }
    if (sh_eq(argv[0], "head") || sh_eq(argv[0], "tail")) {
        int n = 10; const char *source = stdin_text ? stdin_text : ""; int tail = sh_eq(argv[0], "tail");
        if (argc >= 2 && sh_is_num(argv[1])) n = sh_atoi(argv[1]);
        if ((argc >= 2 && !sh_is_num(argv[1])) || argc >= 3) {
            const char *arg = argc >= 3 ? argv[2] : argv[1];
            api->resolve_path(ctx, arg, path, sizeof(path)); sh_slurp(path, buf, sizeof(buf)); source = buf;
        }
        sh_head_tail(source, n, tail, out, max); return 1;
    }
    if (sh_eq(argv[0], "grep")) {
        const char *needle = argc > 1 ? argv[1] : ""; const char *source = stdin_text ? stdin_text : "";
        if (argc > 2) { api->resolve_path(ctx, argv[2], path, sizeof(path)); sh_slurp(path, buf, sizeof(buf)); source = buf; }
        { int len = sh_len(source), start = 0; for (int i = 0; i <= len; i++) if (source[i] == '\n' || source[i] == 0) { char line[512]; int l = i - start; if (l >= (int)sizeof(line)) l = (int)sizeof(line) - 1; k_memset(line, 0, sizeof(line)); if (l > 0) k_memcpy(line, source + start, (size_t)l); if (k_strstr(line, needle)) { sh_cat(out, line, max); sh_cat(out, "\n", max); } start = i + 1; } }
        return 1;
    }
    if (sh_eq(argv[0], "ls")) {
        const char *arg = argc > 1 ? argv[1] : (api->get_cwd ? api->get_cwd(ctx) : "/");
        if (sh_has_glob(arg)) {
            char matches[16][256]; int count = 0; sh_glob_expand(ctx, api, arg, matches, &count, 16);
            for (int i = 0; i < count; i++) { sh_append_mode_owner(matches[i], out, max); sh_cat(out, " ", max); sh_cat(out, matches[i], max); sh_cat(out, "\n", max); }
            return 1;
        }
        api->resolve_path(ctx, arg, path, sizeof(path));
        if (vfs_is_dir(path)) {
            char entries[32][256]; int is_dir[32]; int count = vfs_list_dir(path, entries, is_dir, 32);
            for (int i = 0; i < count; i++) {
                char full[256]; if (sh_eq(path, "/")) { sh_copy(full, "/", sizeof(full)); sh_cat(full, entries[i], sizeof(full)); }
                else { sh_copy(full, path, sizeof(full)); sh_cat(full, "/", sizeof(full)); sh_cat(full, entries[i], sizeof(full)); }
                sh_append_mode_owner(full, out, max); sh_cat(out, " ", max); sh_cat(out, entries[i], max); if (is_dir[i]) sh_cat(out, "/", max); sh_cat(out, "\n", max);
            }
            return 1;
        }
        if (vfs_exists(path)) { sh_append_mode_owner(path, out, max); sh_cat(out, " ", max); sh_cat(out, path, max); sh_cat(out, "\n", max); return 1; }
        sh_cat(out, "ls: path not found\n", max); return 1;
    }
    return 0;
}

static int sh_handle_pipeline(void *ctx, const shell_ext_api_t *api, const char *cmd_line) {
    char work[SH_MAX_CMD]; char input[SH_MAX_OUT]; char output[SH_MAX_OUT];
    char *segments[8]; int segc = 0; int i = 0; int append = 0;
    char out_path[256]; char in_path[256];
    char *redir_out = NULL; char *redir_in = NULL; char *pipep;
    sh_copy(work, cmd_line, sizeof(work)); sh_trim(work);
    out_path[0] = 0; in_path[0] = 0; input[0] = 0; output[0] = 0;

    if (k_strstr(work, ">>")) { append = 1; redir_out = k_strstr(work, ">>"); }
    else if (k_strstr(work, ">")) redir_out = k_strstr(work, ">");
    if (k_strstr(work, "<")) redir_in = k_strstr(work, "<");

    if (redir_out) {
        char tmp[256];
        sh_copy(tmp, redir_out + (append ? 2 : 1), sizeof(tmp)); sh_trim(tmp);
        api->resolve_path(ctx, tmp, out_path, sizeof(out_path));
        *redir_out = 0; sh_trim(work);
    }
    if (redir_in) {
        char tmp[256];
        sh_copy(tmp, redir_in + 1, sizeof(tmp)); sh_trim(tmp);
        api->resolve_path(ctx, tmp, in_path, sizeof(in_path));
        *redir_in = 0; sh_trim(work);
        sh_slurp(in_path, input, sizeof(input));
    }

    segments[segc++] = work;
    while ((pipep = k_strstr(segments[segc - 1], "|")) != NULL && segc < 8) {
        *pipep = 0;
        pipep++;
        segments[segc++] = pipep;
    }
    for (i = 0; i < segc; i++) sh_trim(segments[i]);
    for (i = 0; i < segc; i++) {
        if (!sh_collect_simple(ctx, api, segments[i], i == 0 ? input : output, input, sizeof(input))) return 0;
        sh_copy(output, input, sizeof(output));
    }
    if (out_path[0]) {
        char merged[SH_MAX_OUT];
        if (append && vfs_exists(out_path)) sh_slurp(out_path, merged, sizeof(merged)); else merged[0] = 0;
        sh_cat(merged, output, sizeof(merged));
        (void)vfs_write_file(out_path, merged, (uint32_t)sh_len(merged));
        api->println_color(ctx, "Redirection OK.", 0x64E682u);
    } else {
        sh_render(ctx, api, output);
    }
    return 1;
}

int shell_ext_try_handle(void *ctx, const shell_ext_api_t *api, const char *cmd_line) {
    char work[SH_MAX_CMD]; char *argv[SH_MAX_ARGS]; int argc = 0;
    char out[SH_MAX_OUT]; char path[256]; char groups[128]; char nb[16];
    int background = 0;
    if (!ctx || !api || !cmd_line) return 0;
    sh_copy(work, cmd_line, sizeof(work)); sh_trim(work); if (!work[0]) return 1;
    sh_update_jobs();

    {
        int len = sh_len(work);
        if (len > 0 && work[len - 1] == '&') { background = 1; work[len - 1] = 0; sh_trim(work); }
    }

    if (background) {
        int jid = sh_add_job(work, (uint32_t)((k_strstr(work, "sleep ") == work) ? sh_atoi(work + 6) * 1000 : 2000));
        if (jid >= 0) {
            char line[64]; line[0] = 0; sh_cat(line, "[", sizeof(line)); sh_itoa(jid, nb, sizeof(nb)); sh_cat(line, nb, sizeof(line)); sh_cat(line, "] running", sizeof(line));
            api->println_color(ctx, line, 0x64E682u); return 1;
        }
    }

    if (k_strstr(work, "|") || k_strstr(work, ">") || k_strstr(work, "<") || sh_has_glob(work)) {
        if (sh_handle_pipeline(ctx, api, work)) return 1;
    }

    sh_parse_args(work, argv, &argc); if (argc == 0) return 1;

    if (sh_eq(argv[0], "id") || sh_eq(argv[0], "groups") || sh_eq(argv[0], "lsmod") || sh_eq(argv[0], "grep") || sh_eq(argv[0], "sshd") || sh_eq(argv[0], "cat") || sh_eq(argv[0], "wc") || sh_eq(argv[0], "head") || sh_eq(argv[0], "tail") || sh_eq(argv[0], "ls") || sh_eq(argv[0], "pwd") || sh_eq(argv[0], "whoami") || sh_eq(argv[0], "hostname") || sh_eq(argv[0], "features") || sh_eq(argv[0], "syscalls") || sh_eq(argv[0], "ipc") || sh_eq(argv[0], "pipes") || sh_eq(argv[0], "shm") || sh_eq(argv[0], "elf") || sh_eq(argv[0], "drivers") || sh_eq(argv[0], "lspci") || sh_eq(argv[0], "pci") || sh_eq(argv[0], "lsdev") || sh_eq(argv[0], "sessions") || sh_eq(argv[0], "logs") || sh_eq(argv[0], "journal") || sh_eq(argv[0], "klog")) {
        if (sh_collect_simple(ctx, api, work, NULL, out, sizeof(out))) { sh_render(ctx, api, out); return 1; }
    }

    if (sh_eq(argv[0], "sshd")) {
        if (argc < 2 || sh_eq(argv[1], "status")) { ssh_status_report(out, sizeof(out)); sh_render(ctx, api, out); return 1; }
        if (sh_eq(argv[1], "start")) { ssh_set_daemon_enabled(1); api->println_color(ctx, "sshd started", 0x64E682u); return 1; }
        if (sh_eq(argv[1], "stop")) { ssh_set_daemon_enabled(0); api->println_color(ctx, "sshd stopped", 0x64E682u); return 1; }
        return 1;
    }

    if (sh_eq(argv[0], "ssh")) {
        const char *target = NULL; const char *keyfile = ""; const char *forward = "";
        for (int i = 1; i < argc; i++) {
            if (sh_eq(argv[i], "-i") && i + 1 < argc) { keyfile = argv[++i]; }
            else if (sh_eq(argv[i], "-L") && i + 1 < argc) { forward = argv[++i]; }
            else target = argv[i];
        }
        if (!target) { api->println_color(ctx, "Usage: ssh [-i key] [-L lport:host:rport] user@host", 0xFF9650u); return 1; }
        if (ssh_client_connect(target, keyfile, forward, out, sizeof(out)) == 0) api->println_color(ctx, out, 0x64E682u);
        else api->println_color(ctx, out, 0xFF7860u);
        return 1;
    }

    if (sh_eq(argv[0], "insmod") || sh_eq(argv[0], "modprobe")) {
        if (argc < 2) { api->println_color(ctx, "Usage: insmod <module>", 0xFF9650u); return 1; }
        if ((sh_eq(argv[0], "insmod") ? module_insmod(argv[1], out, sizeof(out)) : module_modprobe(argv[1], out, sizeof(out))) == 0) api->println_color(ctx, out, 0x64E682u);
        else api->println_color(ctx, out, 0xFF7860u);
        return 1;
    }

    if (sh_eq(argv[0], "rmmod")) {
        if (argc < 2) { api->println_color(ctx, "Usage: rmmod <module>", 0xFF9650u); return 1; }
        if (module_rmmod(argv[1], out, sizeof(out)) == 0) api->println_color(ctx, out, 0x64E682u);
        else api->println_color(ctx, out, 0xFF7860u);
        return 1;
    }

    if (sh_eq(argv[0], "chmod")) {
        int mode;
        if (argc < 3) { api->println_color(ctx, "Usage: chmod <mode-octal> <path>", 0xFF9650u); return 1; }
        mode = 0; for (int i = 0; argv[1][i]; i++) if (argv[1][i] >= '0' && argv[1][i] <= '7') mode = (mode << 3) + (argv[1][i] - '0');
        api->resolve_path(ctx, argv[2], path, sizeof(path));
        if (vfs_set_permissions(path, (uint32_t)mode) == 0) api->println_color(ctx, "chmod OK", 0x64E682u); else api->println_color(ctx, "chmod failed", 0xFF7860u);
        return 1;
    }

    if (sh_eq(argv[0], "chown")) {
        char spec[64]; char *colon; int uid = 1000; int gid = 1000; user_t *u;
        if (argc < 3) { api->println_color(ctx, "Usage: chown <user:group> <path>", 0xFF9650u); return 1; }
        sh_copy(spec, argv[1], sizeof(spec)); colon = k_strstr(spec, ":"); if (colon) { *colon = 0; colon++; }
        u = users_find(spec); if (u) uid = u->uid; else if (sh_is_num(spec)) uid = sh_atoi(spec);
        if (colon) { int gg = users_group_gid(colon); gid = gg >= 0 ? gg : (sh_is_num(colon) ? sh_atoi(colon) : gid); }
        api->resolve_path(ctx, argv[2], path, sizeof(path));
        if (vfs_set_owner(path, (uint32_t)uid, (uint32_t)gid) == 0) api->println_color(ctx, "chown OK", 0x64E682u); else api->println_color(ctx, "chown failed", 0xFF7860u);
        return 1;
    }

    if (sh_eq(argv[0], "passwd")) {
        const char *user = api->get_username ? api->get_username(ctx) : "user";
        const char *pass = argc > 1 ? argv[argc - 1] : NULL;
        if (argc == 3) user = argv[1];
        if (!pass || argc < 2) { api->println_color(ctx, "Usage: passwd [user] <newpass>", 0xFF9650u); return 1; }
        if (users_set_password(user, pass)) api->println_color(ctx, "Password updated", 0x64E682u); else api->println_color(ctx, "passwd failed", 0xFF7860u);
        return 1;
    }

    if (sh_eq(argv[0], "su")) {
        if (argc < 2) { api->println_color(ctx, "Usage: su <user> [password]", 0xFF9650u); return 1; }
        if (users_switch_user(argv[1], argc > 2 ? argv[2] : "")) {
            user_t *u = users_get_current();
            if (api->set_username && u) api->set_username(ctx, u->username);
            if (api->set_cwd && u) api->set_cwd(ctx, u->home_dir);
            api->println_color(ctx, "User switched", 0x64E682u);
        } else api->println_color(ctx, "Authentication failed", 0xFF7860u);
        return 1;
    }

    if (sh_eq(argv[0], "jobs")) {
        sh_update_jobs();
        for (int i = 0; i < SH_MAX_JOBS; i++) {
            if (!g_jobs[i].active) continue;
            out[0] = 0; sh_cat(out, "[", sizeof(out)); sh_itoa(g_jobs[i].id, nb, sizeof(nb)); sh_cat(out, nb, sizeof(out)); sh_cat(out, "] ", sizeof(out));
            sh_cat(out, g_jobs[i].done ? "done " : "running ", sizeof(out)); sh_cat(out, g_jobs[i].command, sizeof(out));
            api->println(ctx, out);
        }
        return 1;
    }

    if (sh_eq(argv[0], "fg") || sh_eq(argv[0], "bg")) {
        int id = (argc > 1) ? sh_atoi(argv[1]) : 0; shell_job_t *j = sh_find_job(id);
        if (!j) { api->println_color(ctx, "job not found", 0xFF7860u); return 1; }
        if (sh_eq(argv[0], "fg")) { j->done = 1; api->println_color(ctx, "job completed in foreground", 0x64E682u); }
        else { api->println_color(ctx, "job resumed in background", 0x64E682u); }
        return 1;
    }

    if (sh_eq(argv[0], "sleep")) {
        int seconds = argc > 1 ? sh_atoi(argv[1]) : 1; int jid = sh_add_job(work, (uint32_t)(seconds * 1000));
        if (jid >= 0) api->println_color(ctx, "sleep job scheduled", 0x64E682u); else api->println_color(ctx, "unable to schedule sleep", 0xFF7860u);
        return 1;
    }

    return 0;
}
