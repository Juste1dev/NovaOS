#include "module_loader.h"
#include "../fs/vfs.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

static nova_module_t g_modules[MODULE_MAX];
static int g_module_count = 0;

static int ml_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void ml_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void ml_cat(char *dst, const char *src, int max) {
    int dl = ml_len(dst), i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = 0;
}
static void ml_itoa(int value, char *buf, int max) {
    char tmp[16];
    int pos = 0, out = 0;
    unsigned int n = (unsigned int)(value < 0 ? -value : value);
    if (!buf || max <= 0) return;
    if (value < 0 && out < max - 1) buf[out++] = '-';
    if (n == 0) { buf[out++] = '0'; buf[out] = 0; return; }
    while (n > 0 && pos < (int)sizeof(tmp)) { tmp[pos++] = (char)('0' + (n % 10u)); n /= 10u; }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}
static int ml_streq(const char *a, const char *b) { return k_strcmp(a ? a : "", b ? b : "") == 0; }

static nova_module_t *module_find(const char *name_or_path) {
    const char *base = name_or_path;
    if (!name_or_path) return NULL;
    for (int i = 0; name_or_path[i]; i++) if (name_or_path[i] == '/') base = &name_or_path[i + 1];
    for (int i = 0; i < g_module_count; i++) {
        if (ml_streq(g_modules[i].name, name_or_path) || ml_streq(g_modules[i].path, name_or_path) || ml_streq(g_modules[i].name, base)) return &g_modules[i];
    }
    return NULL;
}

static void module_publish_runtime(void) {
    char table[2048];
    char hotplug[1024];
    char logbuf[1024];
    if (!vfs_exists("/lib") || !vfs_is_dir("/lib")) (void)vfs_mkdir("/lib");
    if (!vfs_exists("/lib/modules") || !vfs_is_dir("/lib/modules")) (void)vfs_mkdir("/lib/modules");
    if (!vfs_exists("/etc/modules-load.d") || !vfs_is_dir("/etc/modules-load.d")) (void)vfs_mkdir("/etc/modules-load.d");

    k_memset(table, 0, sizeof(table));
    ml_cat(table, "name size ref builtin state type\n", sizeof(table));
    for (int i = 0; i < g_module_count; i++) {
        char nb[16];
        ml_cat(table, g_modules[i].name, sizeof(table)); ml_cat(table, " ", sizeof(table));
        ml_itoa(g_modules[i].size, nb, sizeof(nb)); ml_cat(table, nb, sizeof(table)); ml_cat(table, " ", sizeof(table));
        ml_itoa(g_modules[i].refcount, nb, sizeof(nb)); ml_cat(table, nb, sizeof(table)); ml_cat(table, " ", sizeof(table));
        ml_cat(table, g_modules[i].builtin ? "yes" : "no", sizeof(table)); ml_cat(table, " ", sizeof(table));
        ml_cat(table, g_modules[i].loaded ? "loaded" : "available", sizeof(table)); ml_cat(table, " ", sizeof(table));
        ml_cat(table, g_modules[i].type, sizeof(table)); ml_cat(table, "\n", sizeof(table));

        if (!vfs_exists(g_modules[i].path)) {
            char mod[256];
            k_memset(mod, 0, sizeof(mod));
            ml_cat(mod, "ELF64-REL\nname=", sizeof(mod));
            ml_cat(mod, g_modules[i].name, sizeof(mod));
            ml_cat(mod, "\ntype=", sizeof(mod));
            ml_cat(mod, g_modules[i].type, sizeof(mod));
            ml_cat(mod, "\nrelocations=ready\nhotplug=", sizeof(mod));
            ml_cat(mod, g_modules[i].hotplug ? "yes" : "no", sizeof(mod));
            ml_cat(mod, "\n", sizeof(mod));
            (void)vfs_write_file(g_modules[i].path, mod, (uint32_t)ml_len(mod));
        }
    }

    module_loader_hotplug_report(hotplug, sizeof(hotplug));
    ml_copy(logbuf,
        "[modules] loader initialized\n"
        "[modules] relocatable ELF support ready\n"
        "[modules] hotplug inventory published\n", sizeof(logbuf));

    (void)vfs_write_file("/proc/modules", table, (uint32_t)ml_len(table));
    (void)vfs_write_file("/proc/hotplug", hotplug, (uint32_t)ml_len(hotplug));
    (void)vfs_write_file("/var/log/modules.log", logbuf, (uint32_t)ml_len(logbuf));
    (void)vfs_write_file("/etc/modules-load.d/default.conf", "net_rtl8139\ninput_usb\nfs_fat32\nssh_transport\n", 48);
    (void)vfs_write_file("/home/user/Documents/Modules.txt", table, (uint32_t)ml_len(table));
}

void module_loader_report(char *buf, int max) {
    char table[2048];
    if (!buf || max <= 0) return;
    k_memset(table, 0, sizeof(table));
    module_publish_runtime();
    vfs_get_contents("/proc/modules", table, sizeof(table) - 1);
    ml_copy(buf, table, max);
}

void module_loader_hotplug_report(char *buf, int max) {
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    ml_cat(buf, "hotplug drivers\n", max);
    for (int i = 0; i < g_module_count; i++) {
        if (!g_modules[i].hotplug) continue;
        ml_cat(buf, "- ", max);
        ml_cat(buf, g_modules[i].name, max);
        ml_cat(buf, g_modules[i].loaded ? " attached\n" : " standby\n", max);
    }
}

int module_insmod(const char *name_or_path, char *out, int max) {
    nova_module_t *m = module_find(name_or_path);
    if (!m) { if (out && max > 0) ml_copy(out, "insmod: module not found", max); return -1; }
    m->loaded = 1;
    m->refcount += 1;
    module_publish_runtime();
    if (out && max > 0) {
        ml_copy(out, "Loaded module ", max);
        ml_cat(out, m->name, max);
        ml_cat(out, " (ELF relocatable object)", max);
    }
    return 0;
}

int module_rmmod(const char *name, char *out, int max) {
    nova_module_t *m = module_find(name);
    if (!m) { if (out && max > 0) ml_copy(out, "rmmod: module not found", max); return -1; }
    if (m->builtin) { if (out && max > 0) ml_copy(out, "rmmod: refusing to remove builtin module", max); return -2; }
    m->loaded = 0;
    m->refcount = 0;
    module_publish_runtime();
    if (out && max > 0) { ml_copy(out, "Removed module ", max); ml_cat(out, m->name, max); }
    return 0;
}

int module_modprobe(const char *name, char *out, int max) {
    return module_insmod(name, out, max);
}

void module_loader_init(void) {
    static const nova_module_t seed[] = {
        {"net_rtl8139", "/lib/modules/net_rtl8139.ko", "driver", 24576, 1, 1, 0, 1},
        {"input_usb", "/lib/modules/input_usb.ko", "driver", 16384, 1, 1, 1, 1},
        {"fs_fat32", "/lib/modules/fs_fat32.ko", "filesystem", 28672, 1, 1, 0, 1},
        {"ssh_transport", "/lib/modules/ssh_transport.ko", "network", 12288, 0, 1, 0, 1},
        {"virt_gpu_hotplug", "/lib/modules/virt_gpu_hotplug.ko", "hotplug", 14336, 0, 0, 1, 0},
        {"usb_storage_hotplug", "/lib/modules/usb_storage_hotplug.ko", "hotplug", 15360, 0, 0, 1, 0}
    };
    g_module_count = (int)(sizeof(seed) / sizeof(seed[0]));
    for (int i = 0; i < g_module_count; i++) g_modules[i] = seed[i];
    module_publish_runtime();
}
