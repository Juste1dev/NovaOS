#include "nova_pkg.h"

#include "../fs/vfs.h"
#include "../kernel/timer.h"
#include "../libc.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char id[NOVA_SHORTCUT_ID_MAX];
    char name[NOVA_SHORTCUT_NAME_MAX];
    char version[32];
    char launch_type[NOVA_SHORTCUT_TYPE_MAX];
    char launch_target[NOVA_SHORTCUT_TARGET_MAX];
    char description[256];
    char format_ext[16];
    char dependencies[8][NOVA_SHORTCUT_ID_MAX];
    int  dep_count;
    int  desktop_shortcut;
    int  menu_shortcut;
} nova_pkg_meta_t;

#define NOVA_COM2 0x2F8
#define NOVA_STORE_BUF_MAX 4096

static inline void nova_outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t nova_inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

static void nova_serial_init(void) {
    static int initialized = 0;
    if (initialized) return;
    nova_outb(NOVA_COM2 + 1, 0x00); nova_outb(NOVA_COM2 + 3, 0x80); nova_outb(NOVA_COM2 + 0, 0x01); nova_outb(NOVA_COM2 + 1, 0x00); nova_outb(NOVA_COM2 + 3, 0x03); nova_outb(NOVA_COM2 + 2, 0xC7); nova_outb(NOVA_COM2 + 4, 0x0B);
    for (int i = 0; i < 128 && (nova_inb(NOVA_COM2 + 5) & 0x01); i++) (void)nova_inb(NOVA_COM2);
    initialized = 1;
}
static int nova_serial_tx_ready(void) { return nova_inb(NOVA_COM2 + 5) & 0x20; }
static int nova_serial_rx_ready(void) { return nova_inb(NOVA_COM2 + 5) & 0x01; }
static void nova_serial_flush_rx(void) { for (int i = 0; i < 512 && nova_serial_rx_ready(); i++) (void)nova_inb(NOVA_COM2); }
static int nova_serial_putc(char c, uint32_t timeout_ms) { uint32_t start = timer_ms(); while (!nova_serial_tx_ready()) if (timer_ms() - start > timeout_ms) return 0; nova_outb(NOVA_COM2, (uint8_t)c); return 1; }
static int nova_serial_getc(char *c, uint32_t timeout_ms) { uint32_t start = timer_ms(); while (!nova_serial_rx_ready()) if (timer_ms() - start > timeout_ms) return 0; *c = (char)nova_inb(NOVA_COM2); return 1; }
static int nova_serial_read_line(char *buf, int max, uint32_t timeout_ms) {
    int pos = 0; char c = 0; if (!buf || max <= 1) return 0;
    while (pos < max - 1) { if (!nova_serial_getc(&c, timeout_ms)) break; if (c == '\r') continue; if (c == '\n') break; buf[pos++] = c; }
    buf[pos] = 0; return pos;
}

static int nova_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void nova_strcpy(char *dst, const char *src, int max) { int i = 0; if (!dst || max <= 0) return; while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; } dst[i] = 0; }
static void nova_strcat(char *dst, const char *src, int max) { int dl = nova_strlen(dst), i = 0; if (!dst || max <= 0) return; while (src && src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; } dst[dl + i] = 0; }
static int nova_starts_with(const char *s, const char *prefix) { while (*prefix) if (*s++ != *prefix++) return 0; return 1; }
static int nova_strieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}
static int nova_is_true(const char *s) { return nova_strieq(s, "1") || nova_strieq(s, "true") || nova_strieq(s, "yes") || nova_strieq(s, "oui"); }
static void nova_trim(char *s) {
    int start = 0, end; if (!s) return;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) k_memmove(s, s + start, (size_t)(nova_strlen(s + start) + 1));
    end = nova_strlen(s);
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) s[--end] = 0;
}
static void nova_safe_name(const char *src, char *dst, int max) {
    int j = 0; if (!dst || max <= 0) return; if (!src) { dst[0] = 0; return; }
    for (int i = 0; src[i] && j < max - 1; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') dst[j++] = c;
        else dst[j++] = '_';
    }
    dst[j] = 0;
}
static void nova_copy_line(char *dst, int max, const char *start, int len) { if (!dst || max <= 0) return; if (len < 0) len = 0; if (len > max - 1) len = max - 1; for (int i = 0; i < len; i++) dst[i] = start[i]; dst[len] = 0; }
static void nova_split_dependencies(nova_pkg_meta_t *pkg, char *value) {
    int pos = 0;
    if (!pkg || !value) return;
    while (value[pos] && pkg->dep_count < 8) {
        int start = pos;
        char dep[NOVA_SHORTCUT_ID_MAX];
        while (value[pos] && value[pos] != ',') pos++;
        nova_copy_line(dep, sizeof(dep), value + start, pos - start);
        nova_trim(dep);
        if (dep[0]) {
            nova_strcpy(pkg->dependencies[pkg->dep_count], dep, sizeof(pkg->dependencies[pkg->dep_count]));
            pkg->dep_count++;
        }
        if (value[pos] == ',') pos++;
    }
}
static int nova_manifest_header_valid(const char *line, char *ext, int max) {
    if (nova_strieq(line, "NOVA-NOVAPKG-1") || nova_strieq(line, "NOVAPKG-1") || nova_strieq(line, "NOVAPKG1")) {
        nova_strcpy(ext, ".novapkg", max);
        return 1;
    }
    if (nova_strieq(line, "NOVA-PKG-1") || nova_strieq(line, "NOVA1")) {
        nova_strcpy(ext, ".nova", max);
        return 1;
    }
    return 0;
}
static int nova_parse_manifest(const char *text, nova_pkg_meta_t *pkg, char *status, int status_max) {
    int pos = 0; int header_ok = 0; char line[512];
    if (!text || !pkg) { if (status && status_max > 0) nova_strcpy(status, "Manifeste vide", status_max); return 0; }
    k_memset(pkg, 0, sizeof(*pkg)); pkg->desktop_shortcut = 1; pkg->menu_shortcut = 1; nova_strcpy(pkg->format_ext, ".nova", sizeof(pkg->format_ext));
    while (text[pos]) {
        int start = pos;
        while (text[pos] && text[pos] != '\n') pos++;
        nova_copy_line(line, sizeof(line), text + start, pos - start);
        nova_trim(line);
        if (text[pos] == '\n') pos++;
        if (!line[0]) continue;
        if (!header_ok) {
            if (nova_manifest_header_valid(line, pkg->format_ext, sizeof(pkg->format_ext))) { header_ok = 1; continue; }
            if (status && status_max > 0) nova_strcpy(status, "Entete package invalide", status_max);
            return 0;
        }
        char *eq = k_strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *value = eq + 1;
        nova_trim(line); nova_trim(value);
        if (nova_strieq(line, "id")) nova_strcpy(pkg->id, value, sizeof(pkg->id));
        else if (nova_strieq(line, "name")) nova_strcpy(pkg->name, value, sizeof(pkg->name));
        else if (nova_strieq(line, "version")) nova_strcpy(pkg->version, value, sizeof(pkg->version));
        else if (nova_strieq(line, "launch_type")) nova_strcpy(pkg->launch_type, value, sizeof(pkg->launch_type));
        else if (nova_strieq(line, "launch_target")) nova_strcpy(pkg->launch_target, value, sizeof(pkg->launch_target));
        else if (nova_strieq(line, "description")) nova_strcpy(pkg->description, value, sizeof(pkg->description));
        else if (nova_strieq(line, "desktop_shortcut")) pkg->desktop_shortcut = nova_is_true(value);
        else if (nova_strieq(line, "menu_shortcut")) pkg->menu_shortcut = nova_is_true(value);
        else if (nova_strieq(line, "dependencies") || nova_strieq(line, "depends") || nova_strieq(line, "depends_on")) nova_split_dependencies(pkg, value);
    }
    if (!header_ok) { if (status && status_max > 0) nova_strcpy(status, "Entete package manquante", status_max); return 0; }
    if (!pkg->id[0] || !pkg->name[0] || !pkg->launch_type[0] || !pkg->launch_target[0]) {
        if (status && status_max > 0) nova_strcpy(status, "Champs requis manquants", status_max);
        return 0;
    }
    if (!nova_strieq(pkg->launch_type, "url") && !nova_strieq(pkg->launch_type, "binary") && !nova_strieq(pkg->launch_type, "internal")) {
        if (status && status_max > 0) nova_strcpy(status, "launch_type non supporte", status_max);
        return 0;
    }
    if (status && status_max > 0) nova_strcpy(status, "Manifeste package valide", status_max);
    return 1;
}
static void nova_write_shortcut_file(const char *path, const nova_pkg_meta_t *pkg) {
    char data[768];
    k_memset(data, 0, sizeof(data));
    nova_strcpy(data, "NOVA-LINK-1\n", sizeof(data));
    nova_strcat(data, "id=", sizeof(data)); nova_strcat(data, pkg->id, sizeof(data));
    nova_strcat(data, "\nname=", sizeof(data)); nova_strcat(data, pkg->name, sizeof(data));
    nova_strcat(data, "\nlaunch_type=", sizeof(data)); nova_strcat(data, pkg->launch_type, sizeof(data));
    nova_strcat(data, "\nlaunch_target=", sizeof(data)); nova_strcat(data, pkg->launch_target, sizeof(data));
    nova_strcat(data, "\n", sizeof(data));
    (void)vfs_write_file(path, data, (uint32_t)nova_strlen(data));
}
static int nova_pkg_exists_any(const char *safe) {
    char path[160];
    k_memset(path, 0, sizeof(path)); nova_strcpy(path, "/system/packages/", sizeof(path)); nova_strcat(path, safe, sizeof(path)); nova_strcat(path, ".novapkg", sizeof(path));
    if (vfs_exists(path)) return 1;
    k_memset(path, 0, sizeof(path)); nova_strcpy(path, "/system/packages/", sizeof(path)); nova_strcat(path, safe, sizeof(path)); nova_strcat(path, ".nova", sizeof(path));
    return vfs_exists(path);
}
static void nova_manifest_path_for(const char *safe, const char *ext, char *out, int max) {
    if (!out || max <= 0) return;
    out[0] = 0;
    nova_strcpy(out, "/system/packages/", max);
    nova_strcat(out, safe, max);
    nova_strcat(out, ext, max);
}
static void nova_shortcut_paths_for(const char *safe, char *menu_path, int menu_max, char *desktop_path, int desktop_max) {
    if (menu_path && menu_max > 0) { menu_path[0] = 0; nova_strcpy(menu_path, "/system/menu/", menu_max); nova_strcat(menu_path, safe, menu_max); nova_strcat(menu_path, ".nlink", menu_max); }
    if (desktop_path && desktop_max > 0) { desktop_path[0] = 0; nova_strcpy(desktop_path, "/home/user/Desktop/", desktop_max); nova_strcat(desktop_path, safe, desktop_max); nova_strcat(desktop_path, ".nlink", desktop_max); }
}
static void nova_write_pkg_metadata(const nova_pkg_meta_t *pkg, const char *pkg_path) {
    char meta_path[192];
    char text[1024];
    char safe[96];
    if (!pkg || !pkg_path) return;
    nova_safe_name(pkg->id, safe, sizeof(safe));
    k_memset(meta_path, 0, sizeof(meta_path));
    nova_strcpy(meta_path, "/system/pkgdb/", sizeof(meta_path));
    nova_strcat(meta_path, safe, sizeof(meta_path));
    nova_strcat(meta_path, ".meta", sizeof(meta_path));
    k_memset(text, 0, sizeof(text));
    nova_strcpy(text, "id=", sizeof(text)); nova_strcat(text, pkg->id, sizeof(text));
    nova_strcat(text, "\nname=", sizeof(text)); nova_strcat(text, pkg->name, sizeof(text));
    nova_strcat(text, "\nversion=", sizeof(text)); nova_strcat(text, pkg->version[0] ? pkg->version : "1.0", sizeof(text));
    nova_strcat(text, "\npath=", sizeof(text)); nova_strcat(text, pkg_path, sizeof(text));
    nova_strcat(text, "\ndependencies=", sizeof(text));
    for (int i = 0; i < pkg->dep_count; ++i) { if (i) nova_strcat(text, ",", sizeof(text)); nova_strcat(text, pkg->dependencies[i], sizeof(text)); }
    nova_strcat(text, "\n", sizeof(text));
    (void)vfs_write_file(meta_path, text, (uint32_t)nova_strlen(text));
}
static int nova_store_query_local(const char *command, const char *arg, char *out, int max, char *meta, int meta_max);

static int nova_other_pkg_depends_on(const char *package_id) {
    char names[64][256]; int is_dir[64]; int count;
    if (!package_id || !package_id[0] || !vfs_exists("/system/packages")) return 0;
    count = vfs_list_dir("/system/packages", names, is_dir, 64);
    if (count <= 0) return 0;
    for (int i = 0; i < count; ++i) {
        char path[256]; char text[2048]; nova_pkg_meta_t meta; char status[128];
        if (is_dir[i]) continue;
        k_memset(path, 0, sizeof(path)); nova_strcpy(path, "/system/packages/", sizeof(path)); nova_strcat(path, names[i], sizeof(path));
        k_memset(text, 0, sizeof(text)); vfs_get_contents(path, text, sizeof(text) - 1);
        if (!nova_parse_manifest(text, &meta, status, sizeof(status))) continue;
        if (nova_strieq(meta.id, package_id)) continue;
        for (int d = 0; d < meta.dep_count; ++d) if (nova_strieq(meta.dependencies[d], package_id)) return 1;
    }
    return 0;
}

int nova_store_query(const char *command, const char *arg, char *out, int max, char *meta, int meta_max) {
    char header[128]; int expected = 0; int got = 0;
    if (!command || !out || max <= 1) return -1;
    out[0] = 0; if (meta && meta_max > 0) meta[0] = 0;
    nova_serial_init(); nova_serial_flush_rx();
    for (int i = 0; command[i]; i++) if (!nova_serial_putc(command[i], 400)) return nova_store_query_local(command, arg, out, max, meta, meta_max);
    if (arg && arg[0]) { if (!nova_serial_putc(' ', 400)) return nova_store_query_local(command, arg, out, max, meta, meta_max); for (int i = 0; arg[i]; i++) if (!nova_serial_putc(arg[i], 400)) return nova_store_query_local(command, arg, out, max, meta, meta_max); }
    if (!nova_serial_putc('\n', 400)) return nova_store_query_local(command, arg, out, max, meta, meta_max);
    if (!nova_serial_read_line(header, sizeof(header), 12000)) return nova_store_query_local(command, arg, out, max, meta, meta_max);
    if (nova_starts_with(header, "ERR ")) return nova_store_query_local(command, arg, out, max, meta, meta_max);
    if (!nova_starts_with(header, "OK ")) return nova_store_query_local(command, arg, out, max, meta, meta_max);
    expected = 0; for (const char *p = header + 3; *p >= '0' && *p <= '9'; p++) expected = expected * 10 + (*p - '0');
    if (expected <= 0) return nova_store_query_local(command, arg, out, max, meta, meta_max);
    if (expected >= max) expected = max - 1;
    while (got < expected) { char c = 0; if (!nova_serial_getc(&c, 16000)) break; out[got++] = c; }
    out[got] = 0;
    if (got <= 0) return nova_store_query_local(command, arg, out, max, meta, meta_max);
    if (meta && meta_max > 0) nova_strcpy(meta, "Applications via pont QEMU", meta_max);
    return got;
}

static int nova_store_query_local(const char *command, const char *arg, char *out, int max, char *meta, int meta_max) {
    char path[256];
    if (!command || !out || max <= 1) return -1;
    out[0] = 0;
    if (meta && meta_max > 0) meta[0] = 0;
    if (nova_starts_with(command, "STORE SEARCH")) {
        if (!vfs_exists("/system/store/catalog.txt")) return -1;
        vfs_get_contents("/system/store/catalog.txt", out, max - 1);
        if (meta && meta_max > 0) nova_strcpy(meta, "Catalogue local applications", meta_max);
        return nova_strlen(out);
    }
    if (nova_starts_with(command, "STORE INSTALL") && arg && arg[0]) {
        k_memset(path, 0, sizeof(path));
        nova_strcpy(path, "/system/store/packages/", sizeof(path));
        nova_strcat(path, arg, sizeof(path));
        nova_strcat(path, ".nova", sizeof(path));
        if (!vfs_exists(path)) return -1;
        vfs_get_contents(path, out, max - 1);
        if (meta && meta_max > 0) nova_strcpy(meta, "Package local applications", meta_max);
        return nova_strlen(out);
    }
    return -1;
}

int nova_store_download_package(const char *app_id, char *out, int max, char *meta, int meta_max) { return nova_store_query("STORE INSTALL", app_id, out, max, meta, meta_max); }

int nova_pkg_install_from_text(const char *text, char *status, int status_max) {
    nova_pkg_meta_t pkg; char safe[96]; char pkg_path[160]; char menu_path[160]; char desktop_path[192];
    if (!nova_parse_manifest(text, &pkg, status, status_max)) return 0;
    for (int i = 0; i < pkg.dep_count; ++i) {
        char dep_safe[96];
        nova_safe_name(pkg.dependencies[i], dep_safe, sizeof(dep_safe));
        if (!nova_pkg_exists_any(dep_safe)) {
            if (status && status_max > 0) { nova_strcpy(status, "Dependance manquante: ", status_max); nova_strcat(status, pkg.dependencies[i], status_max); }
            return 0;
        }
    }
    if (!vfs_exists("/system")) (void)vfs_mkdir("/system");
    if (!vfs_exists("/system/packages")) (void)vfs_mkdir("/system/packages");
    if (!vfs_exists("/system/menu")) (void)vfs_mkdir("/system/menu");
    if (!vfs_exists("/system/pkgdb")) (void)vfs_mkdir("/system/pkgdb");
    if (!vfs_exists("/home/user/Desktop")) (void)vfs_mkdir("/home/user/Desktop");
    nova_safe_name(pkg.id, safe, sizeof(safe));
    nova_manifest_path_for(safe, pkg.format_ext, pkg_path, sizeof(pkg_path));
    (void)vfs_write_file(pkg_path, text, (uint32_t)nova_strlen(text));
    nova_shortcut_paths_for(safe, menu_path, sizeof(menu_path), desktop_path, sizeof(desktop_path));
    if (pkg.menu_shortcut) nova_write_shortcut_file(menu_path, &pkg);
    if (pkg.desktop_shortcut) nova_write_shortcut_file(desktop_path, &pkg);
    nova_write_pkg_metadata(&pkg, pkg_path);
    if (status && status_max > 0) {
        nova_strcpy(status, "Package installe (format ", status_max);
        nova_strcat(status, pkg.format_ext, status_max);
        nova_strcat(status, ") avec gestion des dependances", status_max);
    }
    return 1;
}

int nova_pkg_remove(const char *package_id, char *status, int status_max) {
    char safe[96]; char path[192]; char menu_path[160]; char desktop_path[192]; char meta_path[192]; int removed = 0;
    if (!package_id || !package_id[0]) { if (status && status_max > 0) nova_strcpy(status, "ID package manquant", status_max); return 0; }
    if (nova_other_pkg_depends_on(package_id)) { if (status && status_max > 0) nova_strcpy(status, "Suppression refusee: dependance inverse detectee", status_max); return 0; }
    nova_safe_name(package_id, safe, sizeof(safe));
    k_memset(path, 0, sizeof(path)); nova_manifest_path_for(safe, ".novapkg", path, sizeof(path)); if (vfs_exists(path) && vfs_delete(path) >= 0) removed = 1;
    k_memset(path, 0, sizeof(path)); nova_manifest_path_for(safe, ".nova", path, sizeof(path)); if (vfs_exists(path) && vfs_delete(path) >= 0) removed = 1;
    nova_shortcut_paths_for(safe, menu_path, sizeof(menu_path), desktop_path, sizeof(desktop_path));
    if (vfs_exists(menu_path)) (void)vfs_delete(menu_path);
    if (vfs_exists(desktop_path)) (void)vfs_delete(desktop_path);
    k_memset(meta_path, 0, sizeof(meta_path)); nova_strcpy(meta_path, "/system/pkgdb/", sizeof(meta_path)); nova_strcat(meta_path, safe, sizeof(meta_path)); nova_strcat(meta_path, ".meta", sizeof(meta_path));
    if (vfs_exists(meta_path)) (void)vfs_delete(meta_path);
    if (status && status_max > 0) nova_strcpy(status, removed ? "Package supprime" : "Package introuvable", status_max);
    return removed;
}

static int nova_parse_shortcut_text(const char *text, nova_shortcut_t *sc) {
    int pos = 0; int header_ok = 0; char line[384]; char ext[16];
    if (!text || !sc) return 0;
    k_memset(sc, 0, sizeof(*sc));
    while (text[pos]) {
        int start = pos; while (text[pos] && text[pos] != '\n') pos++;
        nova_copy_line(line, sizeof(line), text + start, pos - start); nova_trim(line); if (text[pos] == '\n') pos++;
        if (!line[0]) continue;
        if (!header_ok) {
            if (nova_strieq(line, "NOVA-LINK-1") || nova_manifest_header_valid(line, ext, sizeof(ext))) { header_ok = 1; continue; }
            return 0;
        }
        char *eq = k_strchr(line, '='); if (!eq) continue; *eq = 0; char *value = eq + 1; nova_trim(line); nova_trim(value);
        if (nova_strieq(line, "id")) nova_strcpy(sc->package_id, value, sizeof(sc->package_id));
        else if (nova_strieq(line, "name")) nova_strcpy(sc->name, value, sizeof(sc->name));
        else if (nova_strieq(line, "launch_type")) nova_strcpy(sc->launch_type, value, sizeof(sc->launch_type));
        else if (nova_strieq(line, "launch_target")) nova_strcpy(sc->launch_target, value, sizeof(sc->launch_target));
    }
    return sc->name[0] && sc->launch_type[0] && sc->launch_target[0];
}

int nova_pkg_load_shortcuts(const char *dir, nova_shortcut_t *out, int max) {
    char names[32][256]; int is_dir[32]; int total = 0; int count;
    if (!dir || !out || max <= 0) return 0;
    count = vfs_list_dir(dir, names, is_dir, 32); if (count <= 0) return 0;
    for (int i = 0; i < count && total < max; i++) {
        char path[256]; char data[1024]; if (is_dir[i]) continue;
        k_memset(path, 0, sizeof(path)); nova_strcpy(path, dir, sizeof(path)); if (k_strcmp(dir, "/") != 0) nova_strcat(path, "/", sizeof(path)); nova_strcat(path, names[i], sizeof(path));
        k_memset(data, 0, sizeof(data)); vfs_get_contents(path, data, sizeof(data) - 1);
        if (nova_parse_shortcut_text(data, &out[total])) total++;
    }
    return total;
}

int nova_pkg_query_installed(char *out, int max) {
    char names[64][256]; int is_dir[64]; int count;
    if (!out || max <= 0) return 0;
    out[0] = 0;
    if (!vfs_exists("/system/packages")) return 0;
    count = vfs_list_dir("/system/packages", names, is_dir, 64);
    if (count <= 0) return 0;
    for (int i = 0; i < count; ++i) {
        char path[256]; char text[2048]; nova_pkg_meta_t meta; char status[96];
        if (is_dir[i]) continue;
        k_memset(path, 0, sizeof(path)); nova_strcpy(path, "/system/packages/", sizeof(path)); nova_strcat(path, names[i], sizeof(path));
        k_memset(text, 0, sizeof(text)); vfs_get_contents(path, text, sizeof(text) - 1);
        if (!nova_parse_manifest(text, &meta, status, sizeof(status))) continue;
        nova_strcat(out, "- ", max); nova_strcat(out, meta.id, max); nova_strcat(out, " [", max); nova_strcat(out, meta.format_ext, max); nova_strcat(out, "]", max);
        if (meta.dep_count > 0) { nova_strcat(out, " deps=", max); for (int d = 0; d < meta.dep_count; ++d) { if (d) nova_strcat(out, ",", max); nova_strcat(out, meta.dependencies[d], max); } }
        nova_strcat(out, "\n", max);
    }
    return nova_strlen(out);
}

static int nova_count_listed_apps(const char *payload) {
    int count = 0; int at_line_start = 1;
    for (int i = 0; payload && payload[i]; i++) { if (at_line_start && payload[i] == '-') count++; at_line_start = (payload[i] == '\n'); }
    return count;
}

int nova_store_self_test(char *report, int report_max) {
    char search_buf[NOVA_STORE_BUF_MAX]; char pkg_buf[NOVA_STORE_BUF_MAX]; char meta[128]; char status[160]; int listed = 0; int got; int pass = 1;
    if (!report || report_max <= 0) return 0;
    report[0] = 0;
    got = nova_store_query("STORE SEARCH", "", search_buf, sizeof(search_buf), meta, sizeof(meta));
    if (got <= 0) { nova_strcpy(report, "ECHEC: catalogue applications indisponible", report_max); return 0; }
    listed = nova_count_listed_apps(search_buf); if (listed < 10) pass = 0;
    got = nova_store_download_package("org.mozilla.firefox", pkg_buf, sizeof(pkg_buf), meta, sizeof(meta));
    if (got <= 0) { nova_strcpy(report, "ECHEC: package Firefox indisponible", report_max); return 0; }
    if (!nova_pkg_install_from_text(pkg_buf, status, sizeof(status))) { nova_strcpy(report, "ECHEC: package Firefox telecharge mais installation echouee", report_max); return 0; }
    if (!nova_pkg_exists_any("org.mozilla.firefox")) pass = 0;
    if (!vfs_exists("/system/menu/org.mozilla.firefox.nlink")) pass = 0;
    if (!vfs_exists("/home/user/Desktop/org.mozilla.firefox.nlink")) pass = 0;
    if (pass) {
        nova_strcpy(report, "PASS: Applications ", report_max);
        nova_strcat(report, meta, report_max);
        nova_strcat(report, " · catalogue valide · Firefox installable · raccourcis OK", report_max);
        return 1;
    }
    nova_strcpy(report, "ECHEC: verification post-installation incomplete", report_max); return 0;
}
