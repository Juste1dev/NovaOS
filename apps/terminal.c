

#include "../gui/gui.h"
#include "../gui/font.h"
#include "../drivers/vbe.h"
#include "../drivers/keyboard.h"
#include "../drivers/sound.h"
#include "../kernel/timer.h"
#include "../kernel/users.h"
#include "../kernel/userspace.h"
#include "../kernel/platform_features.h"
#include "../fs/vfs.h"
#include "../net/net.h"
#include "symera.h"
#include "shell_ext.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

#define TERM_COLS      80
#define TERM_ROWS      24
#define TERM_BUF_ROWS  260
#define TERM_MAX_CMD   512
#define TERM_HIST_MAX  64
#define TERM_MAX_ARGS  20
#define TERM_TREE_MAX_DEPTH 4

extern uint64_t heap_used(void);
extern uint64_t heap_total(void);
void app_browser_open_url(const char *url);

typedef struct {
    char   buf[TERM_BUF_ROWS][TERM_COLS + 1];
    color_t colors[TERM_BUF_ROWS][TERM_COLS];
    int    row_count;
    int    scroll_top;
    char   cmd[TERM_MAX_CMD];
    int    cmd_len;
    int    cursor;
    char   cwd[256];
    int    hist_idx;
    char   history[TERM_HIST_MAX][TERM_MAX_CMD];
    int    hist_count;
    window_t *win;
    int    win_id;
    int    blink;
    uint32_t blink_time;
    int    need_redraw;
    char   username[64];
} terminal_t;

static terminal_t g_term;
static int term_open = 0;

static void term_itoa_i(int n, char *buf) {
    if (n < 0) {
        *buf++ = '-';
        n = -n;
    }
    char tmp[16];
    int len = 0;
    if (n == 0) tmp[len++] = '0';
    while (n > 0) { tmp[len++] = (char)('0' + (n % 10)); n /= 10; }
    for (int i = len - 1; i >= 0; i--) *buf++ = tmp[i];
    *buf = 0;
}

static void term_utoa_u32(uint32_t n, char *buf) {
    char tmp[16];
    int len = 0;
    if (n == 0) tmp[len++] = '0';
    while (n > 0) { tmp[len++] = (char)('0' + (n % 10)); n /= 10; }
    for (int i = len - 1; i >= 0; i--) *buf++ = tmp[i];
    *buf = 0;
}

static int term_atoi(const char *s) {
    int neg = 0;
    int n = 0;
    if (!s) return 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return neg ? -n : n;
}

static int term_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void term_strcpy(char *d, const char *s, int max) {
    int i = 0;
    if (!d || max <= 0) return;
    while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void term_strcat_n(char *d, const char *s, int max) {
    int dl = term_strlen(d), i = 0;
    while (s && s[i] && dl + i < max - 1) { d[dl + i] = s[i]; i++; }
    d[dl + i] = 0;
}

static int term_str_eq(const char *a, const char *b) {
    while (*a && *b) if (*a++ != *b++) return 0;
    return *a == *b;
}

static int term_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char term_to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static int term_str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (term_to_lower(*a) != term_to_lower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int term_contains_ci(const char *haystack, const char *needle) {
    if (!needle || !needle[0]) return 1;
    if (!haystack) return 0;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && haystack[i + j] && term_to_lower(haystack[i + j]) == term_to_lower(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static int term_window_alive(terminal_t *t) {
    if (!t || !term_open || t->win_id < 0) return 0;
    for (int i = 0; i < gui.window_count; i++) {
        if (gui.windows[i].id == t->win_id) {
            t->win = &gui.windows[i];
            return 1;
        }
    }
    t->win = NULL;
    return 0;
}

static void term_reset_singleton(void) {
    g_term.win = NULL;
    g_term.win_id = -1;
    term_open = 0;
}

static int term_visible_rows(terminal_t *t) {
    int sh = t && t->win ? (t->win->h - TITLE_BAR_H - 8) : ((1080 * 2 / 3) - TITLE_BAR_H - 8);
    int rows = sh / 16;
    if (rows < 4) rows = 4;
    return rows;
}

static int term_max_scroll(terminal_t *t) {
    int rows_vis = term_visible_rows(t);
    int max_scroll = t->row_count - rows_vis;
    return max_scroll > 0 ? max_scroll : 0;
}

static void term_scroll_to_bottom(terminal_t *t) {
    t->scroll_top = term_max_scroll(t);
}

static void term_scroll_by(terminal_t *t, int delta) {
    int max_scroll = term_max_scroll(t);
    t->scroll_top += delta;
    if (t->scroll_top < 0) t->scroll_top = 0;
    if (t->scroll_top > max_scroll) t->scroll_top = max_scroll;
    t->need_redraw = 1;
}

static void term_newline(terminal_t *t) {
    if (t->row_count < TERM_BUF_ROWS - 1) {
        t->row_count++;
    } else {
        for (int i = 0; i < TERM_BUF_ROWS - 1; i++) {
            k_memcpy(t->buf[i], t->buf[i + 1], TERM_COLS + 1);
            k_memcpy(t->colors[i], t->colors[i + 1], TERM_COLS * sizeof(color_t));
        }
    }
    k_memset(t->buf[t->row_count - 1], 0, TERM_COLS + 1);
    for (int c = 0; c < TERM_COLS; c++) t->colors[t->row_count - 1][c] = RGB(200, 210, 230);
    term_scroll_to_bottom(t);
}

static void term_print_color(terminal_t *t, const char *s, color_t col) {
    if (!s) return;
    int row = t->row_count - 1;
    int col_pos = term_strlen(t->buf[row]);
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n' || col_pos >= TERM_COLS - 1) {
            term_newline(t);
            row = t->row_count - 1;
            col_pos = 0;
            if (s[i] == '\n') continue;
        }
        t->buf[row][col_pos] = s[i];
        t->colors[row][col_pos] = col;
        col_pos++;
        t->buf[row][col_pos] = 0;
    }
    t->need_redraw = 1;
}

static void term_print(terminal_t *t, const char *s) { term_print_color(t, s, RGB(200, 210, 230)); }
static void term_println(terminal_t *t, const char *s) { term_print(t, s); term_newline(t); }
static void term_println_color(terminal_t *t, const char *s, color_t col) { term_print_color(t, s, col); term_newline(t); }

static void term_show_prompt(terminal_t *t) {
    term_newline(t);
    int row = t->row_count - 1;
    const char *u = t->username[0] ? t->username : "user";
    const char *host = "desktops";
    const char *cwd = t->cwd[0] ? t->cwd : "/";
    int cp = 0;

    for (int i = 0; u[i] && cp < TERM_COLS - 1; i++) {
        t->buf[row][cp] = u[i];
        t->colors[row][cp] = RGB(100, 220, 130);
        cp++;
    }
    if (cp < TERM_COLS - 1) { t->buf[row][cp] = '@'; t->colors[row][cp] = RGB(180, 180, 180); cp++; }
    for (int i = 0; host[i] && cp < TERM_COLS - 1; i++) {
        t->buf[row][cp] = host[i];
        t->colors[row][cp] = RGB(100, 180, 255);
        cp++;
    }
    if (cp < TERM_COLS - 1) { t->buf[row][cp] = ':'; t->colors[row][cp] = RGB(180, 180, 180); cp++; }
    for (int i = 0; cwd[i] && cp < TERM_COLS - 4; i++) {
        t->buf[row][cp] = cwd[i];
        t->colors[row][cp] = RGB(255, 200, 100);
        cp++;
    }
    if (cp < TERM_COLS - 1) { t->buf[row][cp] = '$'; t->colors[row][cp] = RGB(220, 220, 220); cp++; }
    if (cp < TERM_COLS - 1) { t->buf[row][cp] = ' '; t->colors[row][cp] = RGB(220, 220, 220); cp++; }
    t->buf[row][cp] = 0;
    t->need_redraw = 1;
}

static void term_parse_args(char *cmd, char **argv, int *argc) {
    *argc = 0;
    int i = 0, in_token = 0;
    while (cmd[i] && *argc < TERM_MAX_ARGS - 1) {
        if (cmd[i] == ' ' || cmd[i] == '\t') {
            if (in_token) { cmd[i] = 0; in_token = 0; }
        } else {
            if (!in_token) { argv[(*argc)++] = &cmd[i]; in_token = 1; }
        }
        i++;
    }
    argv[*argc] = NULL;
}

static void term_join_args(char **argv, int start, int argc, char *out, int max) {
    out[0] = 0;
    for (int i = start; i < argc; i++) {
        if (i > start) term_strcat_n(out, " ", max);
        term_strcat_n(out, argv[i], max);
    }
}

static void term_parent_dir(char *path) {
    int len = term_strlen(path);
    if (len <= 1) { path[0] = '/'; path[1] = 0; return; }
    while (len > 1 && path[len - 1] == '/') path[--len] = 0;
    while (len > 1 && path[len - 1] != '/') path[--len] = 0;
    if (len > 1) path[len - 1] = 0;
    if (path[0] == 0) { path[0] = '/'; path[1] = 0; }
}

static void term_resolve_path(terminal_t *t, const char *input, char *out, int max) {
    if (!input || !input[0]) {
        term_strcpy(out, t->cwd, max);
        return;
    }
    if (input[0] == '/') {
        term_strcpy(out, input, max);
    } else if (term_str_eq(input, ".")) {
        term_strcpy(out, t->cwd, max);
    } else if (term_str_eq(input, "..")) {
        term_strcpy(out, t->cwd, max);
        term_parent_dir(out);
    } else if (term_str_eq(t->cwd, "/")) {
        term_strcpy(out, "/", max);
        term_strcat_n(out, input, max);
    } else {
        term_strcpy(out, t->cwd, max);
        term_strcat_n(out, "/", max);
        term_strcat_n(out, input, max);
    }

    int len = term_strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = 0;
}

static void term_print_path_error(terminal_t *t, const char *cmd, const char *path, const char *msg) {
    char buf[320];
    k_memset(buf, 0, sizeof(buf));
    term_strcat_n(buf, cmd, sizeof(buf));
    term_strcat_n(buf, ": ", sizeof(buf));
    term_strcat_n(buf, path, sizeof(buf));
    term_strcat_n(buf, ": ", sizeof(buf));
    term_strcat_n(buf, msg, sizeof(buf));
    term_println_color(t, buf, RGB(255, 100, 80));
}

static void term_print_banner(terminal_t *t) {
}

static void term_print_help(terminal_t *t) {
    term_println_color(t, "=== Terminal ===", RGB(100, 200, 255));
    term_println(t, "Navigation / systeme :");
    term_println(t, "  help, clear, history, exit, pwd, cd, ls, tree, find");
    term_println(t, "  whoami, id, groups, users, hostname, uname, sysinfo, userspace, ps, neofetch");
    term_println(t, "  date, uptime, mem, heap, net, ip, ping, features, syscalls, ipc, pipes, shm, elf, drivers, lspci, lsdev, sessions, klog, journal");
    term_println(t, "  sshd <start|stop|status>, ssh [-i key] [-L lport:host:rport] user@host, lsmod, insmod, rmmod, modprobe");
    term_println(t, "Fichiers :");
    term_println(t, "  cat <file>, head <file> [n], tail <file> [n], stat <path>, wc <file>");
    term_println(t, "  mkdir <dir>, touch <file>, rm <path>, cp <src> <dst>, mv <src> <dst>");
    term_println(t, "  write <file> <texte>, append <file> <texte>, echo <texte>, grep <motif> [fichier]");
    term_println(t, "  chmod <mode> <path>, chown <user:group> <path>, passwd [user] <pass>, su <user> [pass]");
    term_println(t, "Applications / multimedia :");
    term_println(t, "  open <browser|files|settings|tutorial|about|editor|calc|clock|terminal|dashboard|notes|monitor|userspace|commands|apps|release>");
    term_println(t, "  browse <url>, readme, notes, release, apps, dashboard, monitor, userspace, qa, livrable, plan, logs, beep");
    term_println(t, "Commandes rapides :");
    term_println(t, "  cmd <commande naturelle>");
    term_println(t, "  POSIX shell: pipes |, redirections > >> <, jobs/bg/fg, history, globbing * ?");
}

static void term_print_sysinfo(terminal_t *t) {
    char ip[20], mac[20], dns[20], gw[20], nb[32], line[256];
    net_get_ip_str(net_eth0.ip, ip);
    net_get_ip_str(net_eth0.dns, dns);
    net_get_ip_str(net_eth0.gateway, gw);
    net_get_mac_str(net_eth0.mac, mac);

    term_println_color(t, "Apercu systeme", RGB(100, 200, 255));
    term_println(t, "--------------------------------");
    term_println(t, "OS        : x86_64 bare-metal");
    term_println(t, "Kernel    : x86_64 bare-metal");
    term_println(t, "GUI       : VBE/VESA avec fenetres et widgets");
    term_println(t, "Filesystem: FAT32 RAM VFS + profils locaux");
    term_println(t, "Suite OS  : features, syscalls, klog, journal");
    term_println(t, sound_output_backend());

    k_memset(line, 0, sizeof(line));
    term_strcat_n(line, "IP        : ", sizeof(line));
    term_strcat_n(line, ip, sizeof(line));
    term_println(t, line);

    k_memset(line, 0, sizeof(line));
    term_strcat_n(line, "Gateway   : ", sizeof(line));
    term_strcat_n(line, gw, sizeof(line));
    term_println(t, line);

    k_memset(line, 0, sizeof(line));
    term_strcat_n(line, "DNS       : ", sizeof(line));
    term_strcat_n(line, dns, sizeof(line));
    term_println(t, line);

    k_memset(line, 0, sizeof(line));
    term_strcat_n(line, "MAC       : ", sizeof(line));
    term_strcat_n(line, mac, sizeof(line));
    term_println(t, line);

    k_memset(line, 0, sizeof(line));
    term_strcat_n(line, "Memoire   : ", sizeof(line));
    term_itoa_i((int)(heap_used() / 1024), nb); term_strcat_n(line, nb, sizeof(line));
    term_strcat_n(line, " Ko utilises / ", sizeof(line));
    term_itoa_i((int)(heap_total() / 1024), nb); term_strcat_n(line, nb, sizeof(line));
    term_strcat_n(line, " Ko total", sizeof(line));
    term_println(t, line);
}

static void term_print_neofetch(terminal_t *t) {
    term_println_color(t, "system", RGB(100, 200, 255));
}

static void term_print_file_lines(terminal_t *t, const char *path, int max_lines, int tail_mode) {
    static char buf[8192];
    int sz = (int)vfs_get_size(path);
    if (sz < 0) { term_print_path_error(t, tail_mode ? "tail" : "head", path, "Fichier introuvable"); return; }
    if (vfs_is_dir(path)) { term_print_path_error(t, tail_mode ? "tail" : "head", path, "Chemin vers un dossier"); return; }
    if (max_lines <= 0) max_lines = 10;
    if (sz > (int)sizeof(buf) - 1) sz = (int)sizeof(buf) - 1;
    k_memset(buf, 0, sizeof(buf));
    vfs_get_contents(path, buf, sizeof(buf) - 1);
    buf[sz] = 0;

    int total_lines = 0;
    for (int i = 0; i < sz; i++) if (buf[i] == '\n') total_lines++;
    if (sz > 0 && buf[sz - 1] != '\n') total_lines++;

    int start_line = 0;
    if (tail_mode && total_lines > max_lines) start_line = total_lines - max_lines;

    int cur_line = 0;
    int begin = 0;
    for (int i = 0; i <= sz; i++) {
        if (buf[i] == '\n' || buf[i] == 0) {
            if (!tail_mode ? (cur_line < max_lines) : (cur_line >= start_line)) {
                char save = buf[i];
                buf[i] = 0;
                term_println(t, &buf[begin]);
                buf[i] = save;
            }
            cur_line++;
            begin = i + 1;
            if (!tail_mode && cur_line >= max_lines) break;
        }
    }
}

static void term_cmd_cat(terminal_t *t, const char *path) {
    static char cat_buf[8192];
    int sz = (int)vfs_get_size(path);
    if (sz < 0) { term_print_path_error(t, "cat", path, "Fichier introuvable"); return; }
    if (vfs_is_dir(path)) { term_print_path_error(t, "cat", path, "Chemin vers un dossier"); return; }
    if (sz > (int)sizeof(cat_buf) - 1) sz = (int)sizeof(cat_buf) - 1;
    k_memset(cat_buf, 0, sizeof(cat_buf));
    vfs_get_contents(path, cat_buf, sizeof(cat_buf) - 1);
    cat_buf[sz] = 0;
    int start = 0;
    for (int i = 0; i <= sz; i++) {
        if (cat_buf[i] == '\n' || cat_buf[i] == 0) {
            char save = cat_buf[i];
            cat_buf[i] = 0;
            term_println(t, &cat_buf[start]);
            cat_buf[i] = save;
            start = i + 1;
        }
    }
}

static void term_cmd_wc(terminal_t *t, const char *path) {
    static char buf[8192];
    int sz = (int)vfs_get_size(path);
    if (sz < 0) { term_print_path_error(t, "wc", path, "Fichier introuvable"); return; }
    if (sz > (int)sizeof(buf) - 1) sz = (int)sizeof(buf) - 1;
    k_memset(buf, 0, sizeof(buf));
    vfs_get_contents(path, buf, sizeof(buf) - 1);
    buf[sz] = 0;

    int lines = 0, words = 0, in_word = 0;
    for (int i = 0; i < sz; i++) {
        if (buf[i] == '\n') lines++;
        if (term_is_space(buf[i])) in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    if (sz > 0 && buf[sz - 1] != '\n') lines++;

    char out[256], nb[24];
    k_memset(out, 0, sizeof(out));
    term_itoa_i(lines, nb); term_strcat_n(out, nb, sizeof(out));
    term_strcat_n(out, " lignes, ", sizeof(out));
    term_itoa_i(words, nb); term_strcat_n(out, nb, sizeof(out));
    term_strcat_n(out, " mots, ", sizeof(out));
    term_itoa_i(sz, nb); term_strcat_n(out, nb, sizeof(out));
    term_strcat_n(out, " octets", sizeof(out));
    term_println(t, out);
}

static void term_cmd_stat(terminal_t *t, const char *path) {
    vfs_node_t *node = vfs_get_node(path);
    if (!node || !node->valid) { term_print_path_error(t, "stat", path, "Introuvable"); return; }
    char line[320], nb[24];
    term_println_color(t, "=== Stat ===", RGB(100, 200, 255));
    k_memset(line, 0, sizeof(line));
    term_strcat_n(line, "Path : ", sizeof(line)); term_strcat_n(line, node->path, sizeof(line));
    term_println(t, line);
    k_memset(line, 0, sizeof(line));
    term_strcat_n(line, "Type : ", sizeof(line));
    term_strcat_n(line, node->type == VFS_TYPE_DIR ? "directory" : (node->type == VFS_TYPE_FILE ? "file" : "device"), sizeof(line));
    term_println(t, line);
    k_memset(line, 0, sizeof(line));
    term_strcat_n(line, "Nom  : ", sizeof(line)); term_strcat_n(line, node->name, sizeof(line));
    term_println(t, line);
    k_memset(line, 0, sizeof(line));
    term_strcat_n(line, "Taille: ", sizeof(line)); term_itoa_i((int)node->size, nb); term_strcat_n(line, nb, sizeof(line)); term_strcat_n(line, " octets", sizeof(line));
    term_println(t, line);
}

static void term_tree_walk(terminal_t *t, const char *path, int depth, int max_depth) {
    if (depth > max_depth) return;
    char entries[32][256];
    int is_dir[32];
    int count = vfs_list_dir(path, entries, is_dir, 32);
    if (count < 0) return;
    for (int i = 0; i < count; i++) {
        char line[320];
        k_memset(line, 0, sizeof(line));
        for (int d = 0; d < depth; d++) term_strcat_n(line, "  ", sizeof(line));
        term_strcat_n(line, is_dir[i] ? "|- ": "- ", sizeof(line));
        term_strcat_n(line, entries[i], sizeof(line));
        if (is_dir[i]) term_strcat_n(line, "/", sizeof(line));
        term_println_color(t, line, is_dir[i] ? RGB(100, 180, 255) : RGB(200, 210, 230));
        if (is_dir[i]) {
            char child[256];
            if (term_str_eq(path, "/")) {
                term_strcpy(child, "/", sizeof(child));
                term_strcat_n(child, entries[i], sizeof(child));
            } else {
                term_strcpy(child, path, sizeof(child));
                term_strcat_n(child, "/", sizeof(child));
                term_strcat_n(child, entries[i], sizeof(child));
            }
            term_tree_walk(t, child, depth + 1, max_depth);
        }
    }
}

static void term_cmd_find(terminal_t *t, const char *needle) {
    int hits = 0;
    for (int i = 0; i < vfs_node_count; i++) {
        if (!vfs_nodes[i].valid) continue;
        if (term_contains_ci(vfs_nodes[i].path, needle) || term_contains_ci(vfs_nodes[i].name, needle)) {
            term_println_color(t, vfs_nodes[i].path, vfs_nodes[i].type == VFS_TYPE_DIR ? RGB(100, 180, 255) : RGB(200, 210, 230));
            hits++;
        }
    }
    if (!hits) term_println_color(t, "Aucun resultat.", RGB(140, 150, 170));
}

static int term_copy_file(const char *src, const char *dst) {
    static char buf[VFS_MAX_DATA];
    int sz = (int)vfs_get_size(src);
    if (sz < 0 || vfs_is_dir(src)) return -1;
    if (sz > VFS_MAX_DATA - 1) sz = VFS_MAX_DATA - 1;
    k_memset(buf, 0, sizeof(buf));
    vfs_get_contents(src, buf, sizeof(buf) - 1);
    return vfs_write_file(dst, buf, (uint32_t)sz);
}

static int term_write_text_file(const char *path, const char *text, int append) {
    static char buf[VFS_MAX_DATA];
    if (!append) {
        return vfs_write_file(path, text ? text : "", (uint32_t)(text ? term_strlen(text) : 0));
    }
    k_memset(buf, 0, sizeof(buf));
    if (vfs_exists(path) && !vfs_is_dir(path)) vfs_get_contents(path, buf, sizeof(buf) - 1);
    if (term_strlen(buf) + term_strlen(text ? text : "") >= VFS_MAX_DATA - 1) return -1;
    term_strcat_n(buf, text ? text : "", sizeof(buf));
    return vfs_write_file(path, buf, (uint32_t)term_strlen(buf));
}

static void term_api_println(void *ctx, const char *text) { term_println((terminal_t *)ctx, text ? text : ""); }
static void term_api_println_color(void *ctx, const char *text, uint32_t color) { term_println_color((terminal_t *)ctx, text ? text : "", (color_t)color); }
static void term_api_resolve_path(void *ctx, const char *input, char *out, int max) { term_resolve_path((terminal_t *)ctx, input, out, max); }
static const char *term_api_get_cwd(void *ctx) { return ((terminal_t *)ctx)->cwd; }
static const char *term_api_get_username(void *ctx) { return ((terminal_t *)ctx)->username; }
static void term_api_set_cwd(void *ctx, const char *cwd) { term_strcpy(((terminal_t *)ctx)->cwd, (cwd && cwd[0]) ? cwd : "/", 256); }
static void term_api_set_username(void *ctx, const char *username) { term_strcpy(((terminal_t *)ctx)->username, (username && username[0]) ? username : "user", 64); }
static const shell_ext_api_t g_shell_ext_api = {
    term_api_println,
    term_api_println_color,
    term_api_resolve_path,
    term_api_get_cwd,
    term_api_get_username,
    term_api_set_cwd,
    term_api_set_username
};

static void term_cmd_open(terminal_t *t, const char *target) {
    (void)t;
    if (!target || !target[0]) {
        term_println_color(&g_term, "Usage: open <browser|files|settings|tutorial|about|editor|calc|clock|terminal|dashboard|notes|monitor|userspace|commands|apps|release>", RGB(255, 150, 80));
        return;
    }
    if (term_str_ieq(target, "browser") || term_str_ieq(target, "web")) app_browser_open();
    else if (term_str_ieq(target, "files") || term_str_ieq(target, "filemanager")) app_filemanager_open();
    else if (term_str_ieq(target, "settings")) app_settings_open();
    else if (term_str_ieq(target, "tutorial")) app_tutorial_open();
    else if (term_str_ieq(target, "about")) app_about_open();
    else if (term_str_ieq(target, "editor")) app_editor_open();
    else if (term_str_ieq(target, "calc") || term_str_ieq(target, "calculator")) app_calculator_open();
    else if (term_str_ieq(target, "clock")) app_clock_open();
    else if (term_str_ieq(target, "terminal")) app_terminal_open();
    else if (term_str_ieq(target, "hub") || term_str_ieq(target, "nova-hub") || term_str_ieq(target, "dashboard")) app_nova_hub_open();
    else if (term_str_ieq(target, "notes") || term_str_ieq(target, "quicknotes")) app_quick_notes_open();
    else if (term_str_ieq(target, "monitor") || term_str_ieq(target, "dashboard") || term_str_ieq(target, "system")) app_system_monitor_open();
    else if (term_str_ieq(target, "userspace") || term_str_ieq(target, "processes")) app_userspace_open();
    else if (term_str_ieq(target, "symera") || term_str_ieq(target, "ai") || term_str_ieq(target, "commands") || term_str_ieq(target, "cmd")) app_symera_open();
    else if (term_str_ieq(target, "store") || term_str_ieq(target, "nova-store") || term_str_ieq(target, "apps")) app_browser_open_url("nova://store");
    else if (term_str_ieq(target, "release") || term_str_ieq(target, "releases")) app_browser_open_url("nova://release-notes");
    else {
        term_println_color(&g_term, "Application inconnue.", RGB(255, 100, 80));
        return;
    }
    gui_notify("Application ouverte");
}

static void term_exec_cmd(terminal_t *t, char *cmd_line) {
    int len = term_strlen(cmd_line);
    while (len > 0 && (cmd_line[len - 1] == ' ' || cmd_line[len - 1] == '\t')) cmd_line[--len] = 0;
    if (len == 0) return;

    if (t->hist_count < TERM_HIST_MAX) {
        k_memcpy(t->history[t->hist_count], cmd_line, TERM_MAX_CMD);
        t->hist_count++;
    }
    t->hist_idx = t->hist_count;

    if (shell_ext_try_handle(t, &g_shell_ext_api, cmd_line)) return;

    char cmd_copy[TERM_MAX_CMD];
    char *argv[TERM_MAX_ARGS];
    int argc = 0;
    char path1[256], path2[256], joined[TERM_MAX_CMD], ai_reply[768];

    k_memcpy(cmd_copy, cmd_line, TERM_MAX_CMD);
    term_parse_args(cmd_copy, argv, &argc);
    if (argc == 0) return;

    const char *verb = argv[0];

    if (term_str_eq(verb, "help")) {
        term_print_help(t);
    } else if (term_str_eq(verb, "clear")) {
        k_memset(t->buf, 0, sizeof(t->buf));
        for (int r = 0; r < TERM_BUF_ROWS; r++) for (int c = 0; c < TERM_COLS; c++) t->colors[r][c] = RGB(200, 210, 230);
        t->row_count = 1;
        t->scroll_top = 0;
        term_print_banner(t);
    } else if (term_str_eq(verb, "whoami")) {
        user_t *u = users_get_current();
        term_println(t, (u && u->username[0]) ? u->username : (t->username[0] ? t->username : "user"));
    } else if (term_str_eq(verb, "users")) {
        for (int i = 0; i < user_sys.user_count; i++) {
            char line[256];
            k_memset(line, 0, sizeof(line));
            term_strcat_n(line, user_sys.users[i].username, sizeof(line));
            term_strcat_n(line, "  ", sizeof(line));
            term_strcat_n(line, user_sys.users[i].fullname, sizeof(line));
            if (user_sys.current_uid == user_sys.users[i].uid) term_strcat_n(line, "  [session active]", sizeof(line));
            term_println(t, line);
        }
    } else if (term_str_eq(verb, "hostname")) {
        term_cmd_cat(t, "/etc/hostname");
    } else if (term_str_eq(verb, "pwd")) {
        term_println(t, t->cwd[0] ? t->cwd : "/");
    } else if (term_str_eq(verb, "uname")) {
        if (argc > 1 && term_str_eq(argv[1], "-a")) term_println_color(t, "NovaOS 6.0 x86_64 bare-metal SMP FAT32-RAM", RGB(200, 210, 230));
        else term_println_color(t, "NovaOS", RGB(200, 210, 230));
    } else if (term_str_eq(verb, "sysinfo")) {
        term_print_sysinfo(t);
    } else if (term_str_eq(verb, "userspace")) {
        char report[1024];
        userspace_report(report, sizeof(report));
        term_println_color(t, "=== Userspace ===", RGB(100, 200, 255));
        term_println(t, report);
    } else if (term_str_eq(verb, "ps")) {
        char table[1536];
        userspace_process_table(table, sizeof(table));
        term_println_color(t, "=== Processus userspace ===", RGB(100, 200, 255));
        term_println(t, table);
    } else if (term_str_eq(verb, "neofetch")) {
        term_print_neofetch(t);
    } else if (term_str_eq(verb, "date")) {
        char buf[128], nb[8];
        uint32_t sec = timer_ms() / 1000;
        uint32_t h = (sec / 3600) % 24, m = (sec / 60) % 60, s = sec % 60;
        k_memset(buf, 0, sizeof(buf));
        term_strcat_n(buf, "Tue 14 Apr 2026  ", sizeof(buf));
        if (h < 10) term_strcat_n(buf, "0", sizeof(buf));
        term_itoa_i((int)h, nb);
        term_strcat_n(buf, nb, sizeof(buf));
        term_strcat_n(buf, ":", sizeof(buf));
        if (m < 10) term_strcat_n(buf, "0", sizeof(buf));
        term_itoa_i((int)m, nb);
        term_strcat_n(buf, nb, sizeof(buf));
        term_strcat_n(buf, ":", sizeof(buf));
        if (s < 10) term_strcat_n(buf, "0", sizeof(buf));
        term_itoa_i((int)s, nb);
        term_strcat_n(buf, nb, sizeof(buf));
        term_println(t, buf);
    } else if (term_str_eq(verb, "uptime")) {
        uint32_t sec = timer_ms() / 1000;
        char buf[128], nb[16];
        k_memset(buf, 0, sizeof(buf));
        term_strcat_n(buf, "up ", sizeof(buf));
        term_itoa_i((int)(sec / 60), nb); term_strcat_n(buf, nb, sizeof(buf));
        term_strcat_n(buf, " min,  1 utilisateur,  load average: 0.02, 0.03, 0.01", sizeof(buf));
        term_println(t, buf);
    } else if (term_str_eq(verb, "mem")) {
        char buf[256], nb[24], report[1024];
        k_memset(buf, 0, sizeof(buf));
        term_strcat_n(buf, "Memoire: ", sizeof(buf));
        term_itoa_i((int)(heap_used() / 1024), nb); term_strcat_n(buf, nb, sizeof(buf));
        term_strcat_n(buf, " Ko utilises / ", sizeof(buf));
        term_itoa_i((int)(heap_total() / 1024), nb); term_strcat_n(buf, nb, sizeof(buf));
        term_strcat_n(buf, " Ko total", sizeof(buf));
        term_println(t, buf);
        k_memset(report, 0, sizeof(report));
        nova_platform_memory_report(report, sizeof(report));
        term_println(t, report);
    } else if (term_str_eq(verb, "heap")) {
        term_cmd_cat(t, "/proc/heapinfo");
    } else if (term_str_eq(verb, "net") || term_str_eq(verb, "ip")) {
        char buf[160], ip_str[20], mac_str[20], dns[20], gw[20], report[1024];
        term_println_color(t, "Interface reseau:", RGB(100, 200, 255));
        net_get_ip_str(net_eth0.ip, ip_str);
        net_get_ip_str(net_eth0.dns, dns);
        net_get_ip_str(net_eth0.gateway, gw);
        net_get_mac_str(net_eth0.mac, mac_str);
        k_memset(buf, 0, sizeof(buf)); term_strcat_n(buf, "  eth0: IP=", sizeof(buf)); term_strcat_n(buf, ip_str, sizeof(buf)); term_strcat_n(buf, net_eth0.connected ? "  [CONNECTE]" : "  [HORS LIGNE]", sizeof(buf)); term_println(t, buf);
        k_memset(buf, 0, sizeof(buf)); term_strcat_n(buf, "  GW : ", sizeof(buf)); term_strcat_n(buf, gw, sizeof(buf)); term_println(t, buf);
        k_memset(buf, 0, sizeof(buf)); term_strcat_n(buf, "  DNS: ", sizeof(buf)); term_strcat_n(buf, dns, sizeof(buf)); term_println(t, buf);
        k_memset(buf, 0, sizeof(buf)); term_strcat_n(buf, "  MAC: ", sizeof(buf)); term_strcat_n(buf, mac_str, sizeof(buf)); term_println(t, buf);
        k_memset(report, 0, sizeof(report));
        net_feature_report(report, sizeof(report));
        term_println(t, report);
    } else if (term_str_eq(verb, "ping")) {
        if (argc < 2) {
            term_println_color(t, "Usage: ping <hote>", RGB(255, 150, 80));
        } else {
            char buf[256], rt[16];
            k_memset(buf, 0, sizeof(buf));
            term_strcat_n(buf, "PING ", sizeof(buf)); term_strcat_n(buf, argv[1], sizeof(buf));
            term_println_color(t, buf, RGB(100, 200, 255));
            int r = net_ping(argv[1]);
            if (r >= 0) {
                k_memset(buf, 0, sizeof(buf));
                term_strcat_n(buf, "64 octets de ", sizeof(buf)); term_strcat_n(buf, argv[1], sizeof(buf));
                term_strcat_n(buf, ": icmp_seq=1 ttl=64 time=", sizeof(buf)); term_itoa_i(12 + r, rt); term_strcat_n(buf, rt, sizeof(buf)); term_strcat_n(buf, " ms", sizeof(buf));
                term_println_color(t, buf, RGB(100, 230, 130));
            } else {
                term_println_color(t, "Hote inaccessible (timeout)", RGB(255, 100, 80));
            }
        }
    } else if (term_str_eq(verb, "echo")) {
        term_join_args(argv, 1, argc, joined, sizeof(joined));
        term_println(t, joined);
    } else if (term_str_eq(verb, "ls")) {
        const char *raw = (argc > 1) ? argv[1] : t->cwd;
        term_resolve_path(t, raw, path1, sizeof(path1));
        char entries[32][256]; int is_dir[32];
        int count = vfs_list_dir(path1, entries, is_dir, 32);
        if (count < 0) term_print_path_error(t, "ls", path1, "Dossier introuvable");
        else if (count == 0) term_println_color(t, "(dossier vide)", RGB(140, 150, 170));
        else {
            for (int i = 0; i < count; i++) {
                char line[300]; k_memset(line, 0, sizeof(line));
                term_strcat_n(line, entries[i], sizeof(line));
                if (is_dir[i]) term_strcat_n(line, "/", sizeof(line));
                term_println_color(t, line, is_dir[i] ? RGB(100, 180, 255) : RGB(200, 210, 230));
            }
        }
    } else if (term_str_eq(verb, "tree")) {
        const char *raw = (argc > 1) ? argv[1] : t->cwd;
        term_resolve_path(t, raw, path1, sizeof(path1));
        if (!vfs_is_dir(path1)) term_print_path_error(t, "tree", path1, "Dossier introuvable");
        else { term_println_color(t, path1, RGB(100, 200, 255)); term_tree_walk(t, path1, 0, TERM_TREE_MAX_DEPTH); }
    } else if (term_str_eq(verb, "find")) {
        if (argc < 2) term_println_color(t, "Usage: find <motif>", RGB(255, 150, 80));
        else term_cmd_find(t, argv[1]);
    } else if (term_str_eq(verb, "cd")) {
        const char *dest = (argc > 1) ? argv[1] : "/";
        term_resolve_path(t, dest, path1, sizeof(path1));
        if (vfs_is_dir(path1)) term_strcpy(t->cwd, path1, sizeof(t->cwd));
        else term_print_path_error(t, "cd", path1, "Dossier introuvable");
    } else if (term_str_eq(verb, "cat")) {
        if (argc < 2) term_println_color(t, "Usage: cat <fichier>", RGB(255, 150, 80));
        else { term_resolve_path(t, argv[1], path1, sizeof(path1)); term_cmd_cat(t, path1); }
    } else if (term_str_eq(verb, "head")) {
        if (argc < 2) term_println_color(t, "Usage: head <fichier> [n]", RGB(255, 150, 80));
        else { term_resolve_path(t, argv[1], path1, sizeof(path1)); term_print_file_lines(t, path1, argc > 2 ? term_atoi(argv[2]) : 10, 0); }
    } else if (term_str_eq(verb, "tail")) {
        if (argc < 2) term_println_color(t, "Usage: tail <fichier> [n]", RGB(255, 150, 80));
        else { term_resolve_path(t, argv[1], path1, sizeof(path1)); term_print_file_lines(t, path1, argc > 2 ? term_atoi(argv[2]) : 10, 1); }
    } else if (term_str_eq(verb, "wc")) {
        if (argc < 2) term_println_color(t, "Usage: wc <fichier>", RGB(255, 150, 80));
        else { term_resolve_path(t, argv[1], path1, sizeof(path1)); term_cmd_wc(t, path1); }
    } else if (term_str_eq(verb, "stat")) {
        if (argc < 2) term_println_color(t, "Usage: stat <chemin>", RGB(255, 150, 80));
        else { term_resolve_path(t, argv[1], path1, sizeof(path1)); term_cmd_stat(t, path1); }
    } else if (term_str_eq(verb, "mkdir")) {
        if (argc < 2) term_println_color(t, "Usage: mkdir <dossier>", RGB(255, 150, 80));
        else {
            term_resolve_path(t, argv[1], path1, sizeof(path1));
            if (vfs_mkdir(path1) == 0) term_println_color(t, "Dossier cree.", RGB(100, 230, 130));
            else term_println_color(t, "Erreur: impossible de creer le dossier.", RGB(255, 100, 80));
        }
    } else if (term_str_eq(verb, "touch")) {
        if (argc < 2) term_println_color(t, "Usage: touch <fichier>", RGB(255, 150, 80));
        else {
            term_resolve_path(t, argv[1], path1, sizeof(path1));
            if (vfs_write_file(path1, "", 0) >= 0) term_println_color(t, "Fichier cree.", RGB(100, 230, 130));
            else term_println_color(t, "Erreur: impossible de creer le fichier.", RGB(255, 100, 80));
        }
    } else if (term_str_eq(verb, "rm")) {
        if (argc < 2) term_println_color(t, "Usage: rm <chemin>", RGB(255, 150, 80));
        else {
            term_resolve_path(t, argv[1], path1, sizeof(path1));
            if (vfs_delete(path1) >= 0) term_println_color(t, "Entree supprimee.", RGB(100, 230, 130));
            else term_println_color(t, "Erreur: impossible de supprimer.", RGB(255, 100, 80));
        }
    } else if (term_str_eq(verb, "cp")) {
        if (argc < 3) term_println_color(t, "Usage: cp <source> <destination>", RGB(255, 150, 80));
        else {
            term_resolve_path(t, argv[1], path1, sizeof(path1));
            term_resolve_path(t, argv[2], path2, sizeof(path2));
            if (term_copy_file(path1, path2) >= 0) term_println_color(t, "Copie reussie.", RGB(100, 230, 130));
            else term_println_color(t, "Erreur: copie impossible (fichier seulement).", RGB(255, 100, 80));
        }
    } else if (term_str_eq(verb, "mv")) {
        if (argc < 3) term_println_color(t, "Usage: mv <source> <destination>", RGB(255, 150, 80));
        else {
            term_resolve_path(t, argv[1], path1, sizeof(path1));
            term_resolve_path(t, argv[2], path2, sizeof(path2));
            if (term_copy_file(path1, path2) >= 0 && vfs_delete(path1) >= 0) term_println_color(t, "Deplacement reussi.", RGB(100, 230, 130));
            else term_println_color(t, "Erreur: deplacement impossible.", RGB(255, 100, 80));
        }
    } else if (term_str_eq(verb, "write")) {
        if (argc < 3) term_println_color(t, "Usage: write <fichier> <texte>", RGB(255, 150, 80));
        else {
            term_resolve_path(t, argv[1], path1, sizeof(path1));
            term_join_args(argv, 2, argc, joined, sizeof(joined));
            if (term_write_text_file(path1, joined, 0) >= 0) term_println_color(t, "Fichier ecrit.", RGB(100, 230, 130));
            else term_println_color(t, "Erreur: ecriture impossible.", RGB(255, 100, 80));
        }
    } else if (term_str_eq(verb, "append")) {
        if (argc < 3) term_println_color(t, "Usage: append <fichier> <texte>", RGB(255, 150, 80));
        else {
            term_resolve_path(t, argv[1], path1, sizeof(path1));
            term_join_args(argv, 2, argc, joined, sizeof(joined));
            if (term_write_text_file(path1, joined, 1) >= 0) term_println_color(t, "Texte ajoute.", RGB(100, 230, 130));
            else term_println_color(t, "Erreur: ajout impossible.", RGB(255, 100, 80));
        }
    } else if (term_str_eq(verb, "open")) {
        term_cmd_open(t, argc > 1 ? argv[1] : "");
    } else if (term_str_eq(verb, "browse")) {
        term_join_args(argv, 1, argc, joined, sizeof(joined));
        if (!joined[0]) term_strcpy(joined, "about:home", sizeof(joined));
        else if (term_str_ieq(joined, "readme")) term_strcpy(joined, "file:///home/user/readme.txt", sizeof(joined));
        else if (term_str_ieq(joined, "notes")) term_strcpy(joined, "file:///home/user/notes.txt", sizeof(joined));
        else if (term_str_ieq(joined, "release") || term_str_ieq(joined, "changelog")) term_strcpy(joined, "nova://release-notes", sizeof(joined));
        else if (term_str_ieq(joined, "store") || term_str_ieq(joined, "apps")) term_strcpy(joined, "nova://store", sizeof(joined));
        else if (term_str_ieq(joined, "userspace")) term_strcpy(joined, "file:///home/user/Documents/Userspace.txt", sizeof(joined));
        else if (term_str_ieq(joined, "bundle") || term_str_ieq(joined, "livrable")) term_strcpy(joined, "file:///home/user/LIVRABLE.txt", sizeof(joined));
        else if (term_str_ieq(joined, "roadmap") || term_str_ieq(joined, "plan")) term_strcpy(joined, "file:///home/user/Documents/Plan.txt", sizeof(joined));
        else if (term_str_ieq(joined, "qa")) term_strcpy(joined, "file:///home/user/Documents/QA-Checklist.txt", sizeof(joined));
        else if (term_str_ieq(joined, "logs")) term_strcpy(joined, "file:///var/log/system.log", sizeof(joined));
        else if (term_str_ieq(joined, "help")) term_strcpy(joined, "about:help", sizeof(joined));
        else if (term_str_ieq(joined, "net")) term_strcpy(joined, "about:net", sizeof(joined));
        else if (term_str_ieq(joined, "bookmarks")) term_strcpy(joined, "about:bookmarks", sizeof(joined));
        app_browser_open_url(joined);
        gui_notify("Navigation envoyee au navigateur");
    } else if (term_str_eq(verb, "ask") || term_str_eq(verb, "ai") || term_str_eq(verb, "cmd")) {
        term_join_args(argv, 1, argc, joined, sizeof(joined));
        if (!joined[0]) {
            term_println_color(t, "Usage: ask <commande naturelle>", RGB(255, 150, 80));
        } else {
            k_memset(ai_reply, 0, sizeof(ai_reply));
            symera_execute_prompt(joined, ai_reply, sizeof(ai_reply));
            term_println_color(t, ai_reply, RGB(120, 220, 255));
            gui_notify("Commande traitee dans le terminal");
        }
    } else if (term_str_eq(verb, "readme")) {
        term_cmd_cat(t, "/home/user/readme.txt");
    } else if (term_str_eq(verb, "notes")) {
        term_cmd_cat(t, "/home/user/notes.txt");
    } else if (term_str_eq(verb, "hub") || term_str_eq(verb, "dashboard")) {
        app_nova_hub_open();
        gui_notify("Tableau de bord ouvert");
    } else if (term_str_eq(verb, "quicknotes")) {
        app_quick_notes_open();
        gui_notify("Notes ouvertes");
    } else if (term_str_eq(verb, "monitor")) {
        app_system_monitor_open();
        gui_notify("Centre Système ouvert");
        term_println_color(t, "Centre Système ouvert.", RGB(100, 230, 130));
    } else if (term_str_eq(verb, "userspace-app")) {
        app_userspace_open();
        gui_notify("Centre userspace ouvert");
        term_println_color(t, "Centre userspace ouvert.", RGB(100, 230, 130));
    } else if (term_str_eq(verb, "release")) {
        term_cmd_cat(t, "/home/user/CHANGELOG.txt");
    } else if (term_str_eq(verb, "store") || term_str_eq(verb, "apps")) {
        app_browser_open_url("nova://store");
        gui_notify("Applications ouvertes");
        term_println_color(t, "Applications ouvertes dans le navigateur.", RGB(100, 230, 130));
    } else if (term_str_eq(verb, "qa")) {
        term_cmd_cat(t, "/home/user/Documents/QA-Checklist.txt");
    } else if (term_str_eq(verb, "bundle") || term_str_eq(verb, "livrable")) {
        term_cmd_cat(t, "/home/user/LIVRABLE.txt");
    } else if (term_str_eq(verb, "roadmap") || term_str_eq(verb, "plan")) {
        term_cmd_cat(t, "/home/user/Documents/Plan.txt");
    } else if (term_str_eq(verb, "features")) {
        term_cmd_cat(t, "/home/user/Documents/Documentation-Technique.txt");
    } else if (term_str_eq(verb, "syscalls")) {
        term_cmd_cat(t, "/proc/syscalls");
    } else if (term_str_eq(verb, "ipc")) {
        term_cmd_cat(t, "/proc/ipc");
    } else if (term_str_eq(verb, "pipes")) {
        term_cmd_cat(t, "/proc/pipes");
    } else if (term_str_eq(verb, "shm")) {
        term_cmd_cat(t, "/proc/shm");
    } else if (term_str_eq(verb, "elf")) {
        term_cmd_cat(t, "/proc/elf");
    } else if (term_str_eq(verb, "drivers")) {
        term_cmd_cat(t, "/proc/drivers");
    } else if (term_str_eq(verb, "lspci") || term_str_eq(verb, "pci")) {
        term_cmd_cat(t, "/proc/pci");
    } else if (term_str_eq(verb, "lsdev")) {
        term_cmd_cat(t, "/proc/devices");
    } else if (term_str_eq(verb, "sessions")) {
        term_cmd_cat(t, "/var/run/sessions/list");
    } else if (term_str_eq(verb, "klog")) {
        term_print_file_lines(t, "/var/log/kernel.log", 20, 1);
    } else if (term_str_eq(verb, "journal")) {
        term_cmd_cat(t, "/var/log/fs-journal.log");
    } else if (term_str_eq(verb, "logs")) {
        term_print_file_lines(t, "/var/log/system.log", 12, 1);
    } else if (term_str_eq(verb, "beep")) {
        sound_run_self_test();
        gui_notify("Test audio relance");
        term_println_color(t, "Test audio en cours.", RGB(100, 230, 130));
    } else if (term_str_eq(verb, "history")) {
        for (int i = 0; i < t->hist_count; i++) {
            char num[8], line[TERM_MAX_CMD + 12];
            term_itoa_i(i + 1, num);
            k_memset(line, 0, sizeof(line));
            if (i + 1 < 10) term_strcat_n(line, " ", sizeof(line));
            term_strcat_n(line, num, sizeof(line)); term_strcat_n(line, "  ", sizeof(line)); term_strcat_n(line, t->history[i], sizeof(line));
            term_println(t, line);
        }
    } else if (term_str_eq(verb, "exit")) {
        window_t *old = t->win;
        term_reset_singleton();
        if (old) gui_close_window(old);
    } else {
        char buf[256];
        k_memset(buf, 0, sizeof(buf));
        term_strcat_n(buf, verb, sizeof(buf));
        term_strcat_n(buf, ": commande introuvable. Tapez 'help'.", sizeof(buf));
        term_println_color(t, buf, RGB(255, 120, 80));
    }
}

static void term_draw(terminal_t *t) {
    if (!t->win || !t->win->visible) return;

    int wx = t->win->x + 8;
    int wy = t->win->y + TITLE_BAR_H + 4;
    int ww = t->win->w - 16;
    int wh = t->win->h - TITLE_BAR_H - 8;

    vbe_blend_rounded_rect(wx, wy, ww, wh, 8, RGB(14, 18, 28), 245);

    int char_w = 8, char_h = 16;
    int cols_vis = ww / char_w;
    int rows_vis = wh / char_h;
    int max_scroll = term_max_scroll(t);
    int start_row = t->scroll_top;
    int at_bottom = 0;
    if (start_row < 0) start_row = 0;
    if (start_row > max_scroll) start_row = max_scroll;
    at_bottom = (start_row >= max_scroll);

    for (int r = 0; r < rows_vis && (start_row + r) < t->row_count; r++) {
        int src_row = start_row + r;
        int py = wy + 4 + r * char_h;
        int col_len = term_strlen(t->buf[src_row]);
        for (int c = 0; c < col_len && c < cols_vis; c++) {
            font_draw_char(wx + 4 + c * char_w, py, t->buf[src_row][c], t->colors[src_row][c], COLOR_TRANS, FONT_NORMAL);
        }
    }

    if (at_bottom) {
        int cur_row = rows_vis - 1;
        int py_cur = wy + 4 + cur_row * char_h;
        int prompt_len = 0;
        if (t->row_count > 0) prompt_len = term_strlen(t->buf[t->row_count - 1]);
        for (int i = 0; i < t->cmd_len; i++) {
            font_draw_char(wx + 4 + (prompt_len + i) * char_w, py_cur, t->cmd[i], RGB(240, 245, 255), COLOR_TRANS, FONT_NORMAL);
        }

        uint32_t now = timer_ms();
        if (now - t->blink_time > 500) { t->blink = !t->blink; t->blink_time = now; }
        if (t->blink) {
            int cx = wx + 4 + (prompt_len + t->cursor) * char_w;
            vbe_blend_rect(cx, py_cur, 2, char_h, RGB(200, 220, 255), 200);
        }
    } else {
        font_draw_string(wx + ww - 188, wy + wh - 22, "Historique · molette / PgUp / PgDn", RGB(120, 140, 176), COLOR_TRANS, FONT_SMALL);
    }
    t->need_redraw = 0;
}

static void term_key_handler(widget_t *w, gui_event_t *evt) {
    (void)w;
    terminal_t *t = &g_term;
    if (!term_window_alive(t)) return;
    if (evt->type == EVT_SCROLL) {
        term_scroll_by(t, evt->scroll > 0 ? -3 : 3);
        return;
    }
    if (evt->type != EVT_KEYDOWN && evt->type != EVT_CHAR) return;

    key_event_t *k = &evt->key;
    if (k->released) return;

    if (k->scancode == KEY_ENTER) {
        int row = t->row_count - 1;
        int plen = term_strlen(t->buf[row]);
        for (int i = 0; i < t->cmd_len && plen + i < TERM_COLS - 1; i++) {
            t->buf[row][plen + i] = t->cmd[i];
            t->colors[row][plen + i] = RGB(240, 245, 255);
            t->buf[row][plen + i + 1] = 0;
        }
        t->cmd[t->cmd_len] = 0;
        if (t->cmd_len > 0) term_exec_cmd(t, t->cmd);
        t->cmd_len = 0; t->cursor = 0;
        k_memset(t->cmd, 0, TERM_MAX_CMD);
        term_show_prompt(t);
        term_scroll_to_bottom(t);
    } else if (k->scancode == KEY_BACKSPACE) {
        if (t->cursor > 0) {
            t->cursor--;
            for (int i = t->cursor; i < t->cmd_len - 1; i++) t->cmd[i] = t->cmd[i + 1];
            t->cmd[--t->cmd_len] = 0;
        }
    } else if (k->scancode == KEY_LEFT) {
        if (t->cursor > 0) t->cursor--;
    } else if (k->scancode == KEY_RIGHT) {
        if (t->cursor < t->cmd_len) t->cursor++;
    } else if (k->scancode == KEY_PGUP) {
        term_scroll_by(t, -(term_visible_rows(t) - 2));
        return;
    } else if (k->scancode == KEY_PGDN) {
        term_scroll_by(t, term_visible_rows(t) - 2);
        return;
    } else if (k->scancode == KEY_HOME && k->ctrl) {
        t->scroll_top = 0;
        t->need_redraw = 1;
        return;
    } else if (k->scancode == KEY_END && k->ctrl) {
        term_scroll_to_bottom(t);
        t->need_redraw = 1;
        return;
    } else if (k->scancode == KEY_UP) {
        if (t->hist_idx > 0) {
            t->hist_idx--;
            k_memcpy(t->cmd, t->history[t->hist_idx], TERM_MAX_CMD);
            t->cmd_len = term_strlen(t->cmd);
            t->cursor = t->cmd_len;
            term_scroll_to_bottom(t);
        }
    } else if (k->scancode == KEY_DOWN) {
        if (t->hist_idx < t->hist_count) {
            t->hist_idx++;
            if (t->hist_idx < t->hist_count) k_memcpy(t->cmd, t->history[t->hist_idx], TERM_MAX_CMD);
            else k_memset(t->cmd, 0, TERM_MAX_CMD);
            t->cmd_len = term_strlen(t->cmd);
            t->cursor = t->cmd_len;
            term_scroll_to_bottom(t);
        }
    } else if (k->ascii >= 32 && k->ascii < 127) {
        if (t->cmd_len < TERM_MAX_CMD - 2) {
            for (int i = t->cmd_len; i > t->cursor; i--) t->cmd[i] = t->cmd[i - 1];
            t->cmd[t->cursor++] = k->ascii;
            t->cmd_len++;
            t->cmd[t->cmd_len] = 0;
            term_scroll_to_bottom(t);
        }
    }
    t->need_redraw = 1;
}

static void term_paint(window_t *win) { (void)win; term_draw(&g_term); }

void app_terminal_open(void) {
    terminal_t *t = &g_term;
    if (term_window_alive(t)) { gui_focus_window(t->win); return; }
    term_reset_singleton();

    k_memset(t, 0, sizeof(terminal_t));
    term_strcpy(t->cwd, "/home/user", sizeof(t->cwd));
    {
        user_t *u = users_get_current();
        if (u && u->username[0]) term_strcpy(t->username, u->username, sizeof(t->username));
        else term_strcpy(t->username, "user", sizeof(t->username));
    }
    t->row_count = 1;
    t->blink_time = timer_ms();
    t->win_id = -1;

    for (int r = 0; r < TERM_BUF_ROWS; r++) for (int c = 0; c < TERM_COLS; c++) t->colors[r][c] = RGB(200, 210, 230);

    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = sw * 2 / 3;
    int wh = sh * 2 / 3;
    int wx = (sw - ww) / 2;
    int wy = (sh - wh) / 2;

    window_t *win = gui_create_window(wx, wy, ww, wh, "Terminal - NovaOS", WIN_DEFAULT);
    if (!win) return;

    win->bg_color = RGB(14, 18, 28);
    win->on_paint = term_paint;
    win->on_event = term_key_handler;
    t->win = win;
    t->win_id = win->id;

    widget_t *focus_w = gui_add_label(win, 0, 0, 1, 1, "");
    if (focus_w) {
        focus_w->on_keydown = term_key_handler;
        focus_w->focused = 1;
    }

    gui_show_window(win);
    gui_focus_window(win);
    term_open = 1;

    term_print_banner(t);
    term_show_prompt(t);
}
