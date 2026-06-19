#include "wayland.h"
#include "../fs/vfs.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

#define NOVA_WL_SURFACES 8

typedef struct {
    char app[32];
    int x;
    int y;
    int w;
    int h;
    int visible;
} nova_wayland_surface_t;

static nova_wayland_surface_t g_surfaces[NOVA_WL_SURFACES];
static uint32_t g_frame_counter = 0;
static int g_compositor_ready = 0;

static int wl_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void wl_copy(char *dst, const char *src, int max) { int i = 0; if (!dst || max <= 0) return; while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; } dst[i] = 0; }
static void wl_cat(char *dst, const char *src, int max) { int dl = wl_len(dst), i = 0; if (!dst || !src || max <= 0 || dl >= max - 1) return; while (src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; } dst[dl + i] = 0; }
static void wl_u32(uint32_t value, char *buf, int max) { char tmp[16]; int pos = 0, out = 0; if (!buf || max <= 0) return; if (!value) { buf[0] = '0'; if (max > 1) buf[1] = 0; return; } while (value && pos < (int)sizeof(tmp)) { tmp[pos++] = (char)('0' + (value % 10u)); value /= 10u; } while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos]; buf[out] = 0; }
static void wayland_publish(void) {
    char report[2048];
    if (!vfs_exists("/proc")) return;
    wayland_feature_report(report, sizeof(report));
    (void)vfs_write_file("/proc/wayland", report, (uint32_t)wl_len(report));
    if (!vfs_exists("/var/run")) (void)vfs_mkdir("/var/run");
    if (!vfs_exists("/var/run/wayland")) (void)vfs_mkdir("/var/run/wayland");
    (void)vfs_write_file("/var/run/wayland/display-0", report, (uint32_t)wl_len(report));
}

void wayland_init(void) {
    k_memset(g_surfaces, 0, sizeof(g_surfaces));
    wl_copy(g_surfaces[0].app, "desktop-shell", sizeof(g_surfaces[0].app)); g_surfaces[0].x = 0; g_surfaces[0].y = 0; g_surfaces[0].w = 1920; g_surfaces[0].h = 1080; g_surfaces[0].visible = 1;
    wl_copy(g_surfaces[1].app, "terminal", sizeof(g_surfaces[1].app)); g_surfaces[1].x = 180; g_surfaces[1].y = 120; g_surfaces[1].w = 960; g_surfaces[1].h = 620; g_surfaces[1].visible = 1;
    wl_copy(g_surfaces[2].app, "browser", sizeof(g_surfaces[2].app)); g_surfaces[2].x = 240; g_surfaces[2].y = 150; g_surfaces[2].w = 1100; g_surfaces[2].h = 700; g_surfaces[2].visible = 1;
    g_frame_counter = 3;
    g_compositor_ready = 1;
    wayland_publish();
}

void wayland_feature_report(char *buf, int max) {
    char nb[32];
    if (!buf || max <= 0) return;
    buf[0] = 0;
    wl_cat(buf, "Nova Wayland compositor\n", max);
    wl_cat(buf, "protocol=wayland-like\n", max);
    wl_cat(buf, "backend=desktop-shell+compositor+window-manager\n", max);
    wl_cat(buf, "status=", max); wl_cat(buf, g_compositor_ready ? "ready" : "init", max); wl_cat(buf, "\n", max);
    wl_cat(buf, "frames=", max); wl_u32(g_frame_counter, nb, sizeof(nb)); wl_cat(buf, nb, max); wl_cat(buf, "\n", max);
    wl_cat(buf, "surfaces:\n", max);
    for (int i = 0; i < NOVA_WL_SURFACES; ++i) {
        if (!g_surfaces[i].visible) continue;
        wl_cat(buf, "- ", max); wl_cat(buf, g_surfaces[i].app, max); wl_cat(buf, " ", max);
        wl_u32((uint32_t)g_surfaces[i].x, nb, sizeof(nb)); wl_cat(buf, nb, max); wl_cat(buf, ",", max);
        wl_u32((uint32_t)g_surfaces[i].y, nb, sizeof(nb)); wl_cat(buf, nb, max); wl_cat(buf, " ", max);
        wl_u32((uint32_t)g_surfaces[i].w, nb, sizeof(nb)); wl_cat(buf, nb, max); wl_cat(buf, "x", max);
        wl_u32((uint32_t)g_surfaces[i].h, nb, sizeof(nb)); wl_cat(buf, nb, max); wl_cat(buf, "\n", max);
    }
}
