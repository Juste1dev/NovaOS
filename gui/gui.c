

#include "gui.h"
#include "custom_assets.h"
#include "user_wallpapers.h"
#include "font.h"
#include "../drivers/vbe.h"
#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "../kernel/timer.h"
#include "../kernel/memory.h"
#include "../fs/vfs.h"
#include "../net/net.h"
#include "../kernel/users.h"
#include "../apps/nova_pkg.h"
#include <stdint.h>
#include "../libc.h"
#include <stddef.h>

typedef struct {
    char   username_input[64];
    int    username_len;
    char   password_input[64];
    int    password_len;
    int    focus_pass;
    int    error;
    int    attempts;
    uint32_t error_time;
    int    selected_user;
} lockscreen_t;

static lockscreen_t g_lock;
static int g_lockscreen_active = 1;
static int g_session_onboarded = 1;
int gui_wallpaper_variant = 0;
int gui_lockscreen_variant = 0;
int gui_accent_variant = 0;
int gui_taskbar_variant = 0;
static int gui_triwave(int t, int period, int amp);
static color_t gui_accent_primary(void);
static color_t gui_accent_secondary(void);
static color_t gui_accent_soft(void);
static void draw_soft_orb(int cx, int cy, int r, color_t color);
static void gui_apply_window_rect(window_t *win, int nx, int ny, int nw, int nh);
static int taskbar_tray_w(void);
static int taskbar_tray_x(void);
static void gui_request_full_redraw(void);
static void gui_request_cursor_redraw(void);
static void gui_draw_wallpaper_cached(void);
static void gui_swap_cursor_regions(int old_x, int old_y, int new_x, int new_y);
static int gui_point_in_desktop_icon(int x, int y);
static int gui_point_in_visible_window(int x, int y);
static volatile int g_full_redraw_needed = 1;
static volatile int g_cursor_redraw_needed = 1;
static uint8_t *g_wallpaper_cache = NULL;
static uint32_t g_wallpaper_cache_tick = 0;
static int g_wallpaper_cache_valid = 0;
static int g_boot_wallpaper_index = 0;
static int g_boot_lockscreen_index = 1;

static inline void gui_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t gui_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static int gui_bcd_to_bin(int v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

static uint8_t gui_cmos_read(uint8_t reg) {
    gui_outb(0x70, reg);
    return gui_inb(0x71);
}

static uint32_t gui_seed_boot_visuals(void) {
    uint8_t status_b = gui_cmos_read(0x0B);
    int bcd = !(status_b & 0x04);
    int sec = gui_cmos_read(0x00);
    int min = gui_cmos_read(0x02);
    int hour = gui_cmos_read(0x04) & 0x7F;
    int day = gui_cmos_read(0x07);
    int mon = gui_cmos_read(0x08);
    int year = gui_cmos_read(0x09);
    uint32_t seed;

    if (bcd) {
        sec = gui_bcd_to_bin(sec);
        min = gui_bcd_to_bin(min);
        hour = gui_bcd_to_bin(hour);
        day = gui_bcd_to_bin(day);
        mon = gui_bcd_to_bin(mon);
        year = gui_bcd_to_bin(year);
    }

    seed = (uint32_t)(sec + min * 61u + hour * 3607u + day * 4099u + mon * 65537u + year * 131071u);
    seed ^= (uint32_t)timer_ms() * 1103515245u;
    seed ^= 0x9E3779B9u;
    if (!seed) seed = 1u;
    return seed;
}

static void gui_select_boot_visuals(void) {
    int count = user_wallpaper_count();
    uint32_t seed;
    if (count <= 0) return;
    seed = gui_seed_boot_visuals();
    g_boot_wallpaper_index = (int)(seed % (uint32_t)count);
    seed = seed * 1664525u + 1013904223u;
    g_boot_lockscreen_index = (int)(seed % (uint32_t)count);
    if (count > 1 && g_boot_lockscreen_index == g_boot_wallpaper_index) {
        g_boot_lockscreen_index = (g_boot_lockscreen_index + 1) % count;
    }
}

static const ui_bitmap_t *gui_active_wallpaper_bitmap(void) {
    int count = user_wallpaper_count();
    if (count <= 0) return ui_bitmap_wallpaper();
    return user_wallpaper_get(g_boot_wallpaper_index % count);
}

static const ui_bitmap_t *gui_active_lockscreen_bitmap(void) {
    int count = user_wallpaper_count();
    if (count <= 0) return ui_bitmap_wallpaper();
    return user_wallpaper_get(g_boot_lockscreen_index % count);
}

static void gui_pref_append(char *dst, const char *src, int max) {
    int len = (int)k_strlen(dst);
    int i = 0;
    if (!dst || !src || max <= 0) return;
    while (src[i] && len + i < max - 1) {
        dst[len + i] = src[i];
        i++;
    }
    dst[len + i] = 0;
}

static int gui_pref_line_value(const char *text, const char *key) {
    int key_len = (int)k_strlen(key);
    int i = 0;
    if (!text || !key || key_len <= 0) return -1;
    while (text[i]) {
        int line_start = i;
        while (text[i] && text[i] != '\n' && text[i] != '\r') i++;
        if (k_strncmp(text + line_start, key, (size_t)key_len) == 0 && text[line_start + key_len] == '=') {
            int sign = 1;
            int v = 0;
            int p = line_start + key_len + 1;
            if (text[p] == '-') { sign = -1; p++; }
            while (text[p] >= '0' && text[p] <= '9') {
                v = v * 10 + (text[p] - '0');
                p++;
            }
            return v * sign;
        }
        while (text[i] == '\n' || text[i] == '\r') i++;
    }
    return -1;
}

void gui_preferences_load(void) {
    char prefs[256];
    prefs[0] = 0;
    if (!vfs_exists("/etc/nova-ui.conf")) return;
    vfs_get_contents("/etc/nova-ui.conf", prefs, sizeof(prefs) - 1);

    { int v = gui_pref_line_value(prefs, "wallpaper"); if (v >= 0 && v < 3) gui_wallpaper_variant = v; }
    { int v = gui_pref_line_value(prefs, "accent"); if (v >= 0 && v < 3) gui_accent_variant = v; }
    { int v = gui_pref_line_value(prefs, "lockscreen"); if (v >= 0 && v < 3) gui_lockscreen_variant = v; }
    { int v = gui_pref_line_value(prefs, "taskbar"); if (v >= 0 && v < 3) gui_taskbar_variant = v; }
    { int v = gui_pref_line_value(prefs, "mouse"); if (v >= 0 && v < 3) mouse_set_speed_preset(v); }
}

void gui_preferences_save(void) {
    char prefs[192];
    char num[16];
    prefs[0] = 0;

    k_strncpy(prefs, "wallpaper=", sizeof(prefs));
    gui_int_to_str(gui_wallpaper_variant, num);
    gui_pref_append(prefs, num, sizeof(prefs));
    gui_pref_append(prefs, "\naccent=", sizeof(prefs));
    gui_int_to_str(gui_accent_variant, num);
    gui_pref_append(prefs, num, sizeof(prefs));
    gui_pref_append(prefs, "\nlockscreen=", sizeof(prefs));
    gui_int_to_str(gui_lockscreen_variant, num);
    gui_pref_append(prefs, num, sizeof(prefs));
    gui_pref_append(prefs, "\ntaskbar=", sizeof(prefs));
    gui_int_to_str(gui_taskbar_variant, num);
    gui_pref_append(prefs, num, sizeof(prefs));
    gui_pref_append(prefs, "\nmouse=", sizeof(prefs));
    gui_int_to_str(mouse_get_speed_preset(), num);
    gui_pref_append(prefs, num, sizeof(prefs));
    gui_pref_append(prefs, "\n", sizeof(prefs));
    vfs_write_file("/etc/nova-ui.conf", prefs, (uint32_t)k_strlen(prefs));
}

static void lockscreen_draw(void) {
    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    const ui_bitmap_t *bg = gui_active_lockscreen_bitmap();
    color_t accent = gui_accent_primary();
    color_t accent2 = gui_accent_secondary();
    color_t border = RGB(72, 94, 128);

    if (bg) ui_draw_bitmap_scaled(bg, 0, 0, sw, sh);
    else vbe_gradient_v(0, 0, sw, sh, RGB(16,20,28), RGB(6,8,14));

    vbe_blend_rect(0, 0, sw, sh, RGB(4, 6, 10), 152);
    vbe_blend_rect(0, 0, sw, sh / 3, RGB(0, 0, 0), 72);

    {
        int t = (int)timer_ms();
        draw_soft_orb(sw / 2 - 360 + gui_triwave(t, 9000, 22), sh / 2 - 220, 250, accent);
        draw_soft_orb(sw / 2 + 260 + gui_triwave(t + 700, 11200, 24), sh / 2 - 10, 220, accent2);
        draw_soft_orb(sw / 2 + 360 + gui_triwave(t + 1400, 8300, 20), sh / 2 + 180, 180, RGB(62, 112, 170));
    }

    vbe_blend_rounded_rect(52, 48, 600, 122, 34, RGB(8, 12, 18), 190);
    vbe_rounded_rect_outline(52, 48, 600, 122, 34, 1, border);
    vbe_gradient_h(84, 82, 190, 12, accent, accent2);
    font_draw_string(84, 98, "NovaOS", COLOR_WHITE, COLOR_TRANS, FONT_TITLE);
    font_draw_string(86, 142, "Session locale  ·  personnalisation au demarrage", RGB(190, 202, 222), COLOR_TRANS, FONT_NORMAL);

    {
        uint32_t sec = timer_ms() / 1000;
        int h = (int)((sec / 3600) % 24);
        int m2 = (int)((sec / 60) % 60);
        char timebuf[8];
        timebuf[0] = '0' + (h / 10);
        timebuf[1] = '0' + (h % 10);
        timebuf[2] = ':';
        timebuf[3] = '0' + (m2 / 10);
        timebuf[4] = '0' + (m2 % 10);
        timebuf[5] = 0;
        font_draw_string(84, sh - 248, timebuf, COLOR_WHITE, COLOR_TRANS, FONT_TITLE);
    }
    font_draw_string(88, sh - 196, "Session securisee", RGB(198, 210, 228), COLOR_TRANS, FONT_LARGE);
    font_draw_string(88, sh - 160, "Entree pour ouvrir  ·  Echap pour effacer", RGB(138, 156, 184), COLOR_TRANS, FONT_SMALL);

    {
        int cw = 500, ch = 410;
        int card_x = sw - cw - 136;
        int card_y = sh / 2 - 210;
        const char *uname = g_lock.username_input[0] ? g_lock.username_input : "user";
        char initial[2];
        int fy;
        int by;
        int pl;
        int dotx;

        if (card_x < 80) card_x = (sw - cw) / 2;

        vbe_blend_rounded_rect(card_x + 10, card_y + 18, cw, ch, 38, RGB(0,0,0), 34);
        vbe_glass_rect(card_x, card_y, cw, ch, 38, RGB(12, 18, 28), 220);
        vbe_blend_rect(card_x + 1, card_y + 1, cw - 2, 84, RGB(18, 24, 36), 235);
        vbe_rounded_rect_outline(card_x, card_y, cw, ch, 38, 1, border);

        font_draw_string(card_x + 40, card_y + 30, "Connexion", COLOR_WHITE, COLOR_TRANS, FONT_LARGE);
        font_draw_string(card_x + 40, card_y + 60, "Connexion locale", RGB(152, 170, 194), COLOR_TRANS, FONT_SMALL);

        initial[0] = uname[0] ? uname[0] : 'U';
        initial[1] = 0;

        vbe_blend_rounded_rect(card_x + 40, card_y + 106, 84, 84, 42, accent, 224);
        vbe_blend_rounded_rect(card_x + 52, card_y + 118, 60, 60, 30, RGB(10, 14, 22), 48);
        font_draw_string_shadow(card_x + 68, card_y + 132, initial, COLOR_WHITE, FONT_TITLE);
        font_draw_string(card_x + 150, card_y + 118, uname, COLOR_WHITE, COLOR_TRANS, FONT_LARGE);
        font_draw_string(card_x + 150, card_y + 152, "Compte principal", RGB(156, 174, 198), COLOR_TRANS, FONT_SMALL);

        fy = card_y + 212;
        vbe_glass_rect(card_x + 38, fy, cw - 76, 56, 18, RGB(18, 24, 36), 226);
        vbe_rounded_rect_outline(card_x + 38, fy, cw - 76, 56, 18, 1, border);
        vbe_blend_rect(card_x + 40, fy + 1, cw - 80, 16, COLOR_WHITE, 10);

        pl = g_lock.password_len;
        dotx = card_x + cw / 2 - (pl * 14) / 2;
        for (int i = 0; i < pl; i++) vbe_circle_fill(dotx + i * 14 + 6, fy + 28, 4, RGB(120, 190, 255));
        if (pl == 0) font_draw_string(card_x + 56, fy + 20, "Mot de passe", RGB(118, 136, 164), COLOR_TRANS, FONT_NORMAL);

        by = fy + 84;
        vbe_blend_rounded_rect(card_x + 38, by, cw - 76, 54, 18, accent, 236);
        vbe_blend_rect(card_x + 40, by + 1, cw - 80, 18, COLOR_WHITE, 18);
        vbe_rounded_rect_outline(card_x + 38, by, cw - 76, 54, 18, 1, gui_accent_soft());
        font_draw_string(card_x + cw / 2 - 74, by + 18, "Ouvrir la session", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
        font_draw_string(card_x + 40, by + 74, "Fond d'ecran personnalise", RGB(148, 166, 192), COLOR_TRANS, FONT_SMALL);

        if (g_lock.error) {
            uint32_t age = timer_ms() - g_lock.error_time;
            if (age < 3000) font_draw_string(card_x + 40, by + 100, "Mot de passe incorrect", RGB(244, 104, 110), COLOR_TRANS, FONT_NORMAL);
            else g_lock.error = 0;
        }

        if (user_sys.user_count > 1) {
            int nusers = user_sys.user_count;
            int chips_w = nusers * 100;
            int ux0 = card_x + cw / 2 - chips_w / 2;
            int uy = card_y + ch + 24;
            for (int i = 0; i < nusers && i < 6; i++) {
                int ux = ux0 + i * 100;
                int sel = (g_lock.selected_user == i);
                vbe_blend_rounded_rect(ux, uy, 88, 40, 20, sel ? accent : RGB(18, 24, 36), sel ? 236 : 216);
                vbe_rounded_rect_outline(ux, uy, 88, 40, 20, 1, sel ? gui_accent_soft() : border);
                font_draw_string(ux + 12, uy + 14, user_sys.users[i].username, sel ? COLOR_WHITE : RGB(188, 198, 214), COLOR_TRANS, FONT_SMALL);
            }
        }
    }
}

static int lockscreen_handle_key(key_event_t *k) {
    if (!g_lockscreen_active) return 0;
    if (k->released) return 1;

    if (k->scancode == KEY_UP) {
        g_lock.selected_user--;
        if (g_lock.selected_user < 0) g_lock.selected_user = user_sys.user_count - 1;
        int i = g_lock.selected_user;
        if (i >= 0 && i < user_sys.user_count) {
            k_memcpy(g_lock.username_input, user_sys.users[i].username, 64);
            g_lock.username_len = (int)k_strlen(user_sys.users[i].username);
        }
        k_memset(g_lock.password_input, 0, 64);
        g_lock.password_len = 0;
        return 1;
    }
    if (k->scancode == KEY_DOWN) {
        g_lock.selected_user = (g_lock.selected_user + 1) % user_sys.user_count;
        int i = g_lock.selected_user;
        if (i >= 0 && i < user_sys.user_count) {
            k_memcpy(g_lock.username_input, user_sys.users[i].username, 64);
            g_lock.username_len = (int)k_strlen(user_sys.users[i].username);
        }
        k_memset(g_lock.password_input, 0, 64);
        g_lock.password_len = 0;
        return 1;
    }

    if (k->scancode == KEY_ENTER) {

        const char *uname = g_lock.username_input[0] ? g_lock.username_input : "user";
        g_lock.password_input[g_lock.password_len] = 0;
        int ok = users_authenticate(uname, g_lock.password_input);
        if (!ok && g_lock.password_len == 0) {

            ok = users_authenticate("guest", "");
        }
        if (ok) {
            g_lockscreen_active = 0;
            users_unlock_screen();
            k_memset(&g_lock, 0, sizeof(g_lock));
            if (!vfs_exists("/etc/install.done")) {
                app_installer_open();
            } else if (!g_session_onboarded) {
                app_tutorial_open();
                g_session_onboarded = 1;
            }
        } else {
            g_lock.error = 1;
            g_lock.error_time = timer_ms();
            g_lock.attempts++;
            k_memset(g_lock.password_input, 0, 64);
            g_lock.password_len = 0;
        }
        return 1;
    }
    if (k->scancode == KEY_BACKSPACE) {
        if (g_lock.password_len > 0)
            g_lock.password_input[--g_lock.password_len] = 0;
        return 1;
    }
    if (k->scancode == KEY_ESC) {
        k_memset(g_lock.password_input, 0, 64);
        g_lock.password_len = 0;
        return 1;
    }
    if (k->ascii >= 32 && k->ascii < 127) {
        if (g_lock.password_len < 63) {
            g_lock.password_input[g_lock.password_len++] = k->ascii;
        }
        return 1;
    }
    return 1;
}

gui_state_t gui;

void gui_string_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max-1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int gui_string_len(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

void gui_int_to_str(int n, char *buf) {
    if (n < 0) { buf[0]='-'; gui_int_to_str(-n, buf+1); return; }
    if (n == 0) { buf[0]='0'; buf[1]=0; return; }
    char tmp[16]; int i=0;
    while (n > 0) { tmp[i++] = '0' + (n%10); n /= 10; }
    int j = 0;
    while (i-- > 0) buf[j++] = tmp[i+1];
    buf[j] = 0;
}

void gui_itoa(int n, char *buf) {
    if (n < 0) { *buf++ = '-'; n = -n; }
    char tmp[12]; int len = 0;
    if (n == 0) { tmp[len++] = '0'; }
    while (n > 0) { tmp[len++] = '0' + (n % 10); n /= 10; }
    for (int i = len-1; i >= 0; i--) *buf++ = tmp[i];
    *buf = 0;
}

static int str_equal(const char *a, const char *b) {
    while (*a && *b) if (*a++ != *b++) return 0;
    return *a == *b;
}

static int gui_screen_w(void) { return vbe.width  ? (int)vbe.width  : SCREEN_WIDTH; }
static int gui_screen_h(void) { return vbe.height ? (int)vbe.height : SCREEN_HEIGHT; }

static color_t gui_accent_primary(void) {
    static const color_t accents[] = {RGB(98,142,255), RGB(132,102,255), RGB(64,196,150)};
    int idx = gui_accent_variant;
    if (idx < 0 || idx >= 3) idx = 0;
    return accents[idx];
}

static color_t gui_accent_secondary(void) {
    static const color_t accents[] = {RGB(116,210,255), RGB(206,124,255), RGB(124,232,188)};
    int idx = gui_accent_variant;
    if (idx < 0 || idx >= 3) idx = 0;
    return accents[idx];
}

static color_t gui_accent_soft(void) {
    static const color_t accents[] = {RGB(210,226,255), RGB(232,220,255), RGB(214,246,232)};
    int idx = gui_accent_variant;
    if (idx < 0 || idx >= 3) idx = 0;
    return accents[idx];
}

static color_t gui_taskbar_shell_color(void) {
    static const color_t fills[] = {RGB(12,20,36), RGB(24,24,30), RGB(236,242,252)};
    int idx = gui_taskbar_variant;
    if (idx < 0 || idx >= 3) idx = 0;
    return fills[idx];
}

static color_t gui_taskbar_outline_color(void) {
    static const color_t outlines[] = {RGB(78,98,136), RGB(112,118,138), RGB(166,186,222)};
    int idx = gui_taskbar_variant;
    if (idx < 0 || idx >= 3) idx = 0;
    return blend_color(gui_accent_secondary(), outlines[idx], 110);
}

static color_t gui_taskbar_text_color(void) {
    return gui_taskbar_variant == 2 ? RGB(36,52,84) : COLOR_WHITE;
}

static color_t gui_taskbar_subtext_color(void) {
    return gui_taskbar_variant == 2 ? RGB(78,102,138) : RGB(186,202,232);
}

static int taskbar_tray_w(void) {
    return 188;
}

static int taskbar_tray_x(void) {
    return gui_screen_w() - taskbar_tray_w() - 18;
}

static int gui_triwave(int t, int period, int amp) {
    if (period < 2) return 0;
    int p = t % period;
    int h = period / 2;
    if (p < h) return -amp + (p * (amp * 2)) / (h ? h : 1);
    return amp - ((p - h) * (amp * 2)) / ((period - h) ? (period - h) : 1);
}

static void draw_shell_logo(int x, int y, int size, color_t c1, color_t c2) {
    int gap = size / 8;
    if (gap < 2) gap = 2;
    int cell = (size - gap) / 2;
    int r = cell / 5;
    if (r < 3) r = 3;
    vbe_blend_rounded_rect(x, y, cell, cell, r, c1, 238);
    vbe_blend_rounded_rect(x + cell + gap, y, cell, cell, r, c2, 238);
    vbe_blend_rounded_rect(x, y + cell + gap, cell, cell, r, c2, 238);
    vbe_blend_rounded_rect(x + cell + gap, y + cell + gap, cell, cell, r, c1, 238);
}

static int count_visible_windows(void) {
    int n = 0;
    for (int i = 0; i < gui.window_count; i++) if (gui.windows[i].visible) n++;
    return n;
}

static int taskbar_window_slots(void) {
    int n = count_visible_windows();
    if (n > 6) n = 6;
    return n;
}

static int taskbar_group_w(void) {
    return 56 + taskbar_window_slots() * 54;
}

static int taskbar_group_left(void) {
    int sw = gui_screen_w();
    int gw = taskbar_group_w();
    if (gw < 56) gw = 56;
    return (sw - gw) / 2;
}

static int taskbar_start_left(void) {
    return taskbar_group_left();
}

static int taskbar_windows_left(void) {
    return taskbar_group_left() + 58;
}

static void start_menu_bounds(int *x, int *y, int *w, int *h) {
    int sw = gui_screen_w();
    int sh = gui_screen_h();
    *w = 620;
    *h = 488;
    *x = (sw - *w) / 2;
    *y = sh - TASKBAR_H - *h - 18;
    if (*y < 28) *y = 28;
}

static const char *ui_symbol_for_name(const char *name) {
    if (str_equal(name, "Terminal")) return ">";
    if (str_equal(name, "Editeur")) return "A";
    if (str_equal(name, "Fichiers")) return "F";
    if (str_equal(name, "Calculatrice")) return "+";
    if (str_equal(name, "Navigateur") || str_equal(name, "Navigateur Web")) return "W";
    if (str_equal(name, "Applications")) return "A";
    if (str_equal(name, "Parametres")) return "S";
    if (str_equal(name, "Horloge")) return "O";
    if (str_equal(name, "System Monitor")) return "M";
    if (str_equal(name, "Userspace")) return "U";
    if (str_equal(name, "Installer")) return "I";
    if (str_equal(name, "Tutoriel")) return "G";
    if (str_equal(name, "A propos")) return "i";
    if (str_equal(name, "Tableau de bord")) return "D";
    if (str_equal(name, "Notes")) return "N";
    if (str_equal(name, "Commandes")) return "C";
    return name && name[0] ? name : "?";
}

static void draw_soft_orb(int cx, int cy, int r, color_t color) {
    int rr = r * r;
    color_t rim = blend_color(COLOR_WHITE, color, 86);
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx*dx + dy*dy;
            if (d2 > rr) continue;
            int falloff = 255 - (d2 * 255 / (rr ? rr : 1));
            if (falloff < 6) continue;

            color_t bg = vbe_get_pixel(x, y);
            color_t mixed = blend_color(color, bg, (uint8_t)(falloff / 5 + 12));

            if (d2 > (rr * 76) / 100) {
                int rim_alpha = (d2 - (rr * 76) / 100) * 255 / ((rr * 24) / 100 ? (rr * 24) / 100 : 1);
                if (rim_alpha > 110) rim_alpha = 110;
                mixed = blend_color(rim, mixed, (uint8_t)rim_alpha);
            }

            int hx = dx + r/3;
            int hy = dy + r/3;
            if (hx*hx*5 + hy*hy*9 < rr) {
                mixed = blend_color(COLOR_WHITE, mixed, 18);
            }
            vbe_put_pixel(x, y, mixed);
        }
    }
}

static void gui_request_full_redraw(void) {
    g_full_redraw_needed = 1;
    g_cursor_redraw_needed = 1;
}

static void gui_request_cursor_redraw(void) {
    g_cursor_redraw_needed = 1;
}

static void draw_wallpaper(void) {
    int sw = gui_screen_w();
    int desktop_h = gui_screen_h() - TASKBAR_H;
    uint32_t t = timer_ms();
    const ui_bitmap_t *bg = gui_active_wallpaper_bitmap();
    color_t accent = gui_accent_primary();
    color_t accent2 = gui_accent_secondary();
    int hero_w = 620;
    int hero_h = 306;
    int hero_x;
    int hero_y;

    if (bg) ui_draw_bitmap_scaled(bg, 0, 0, sw, desktop_h);
    else vbe_gradient_v(0, 0, sw, desktop_h, RGB(16,20,28), RGB(6,8,14));

    vbe_blend_rect(0, 0, sw, desktop_h, RGB(4, 6, 10), 118);
    vbe_blend_rect(0, 0, sw, desktop_h / 4, RGB(0, 0, 0), 48);

    draw_soft_orb(sw / 2 - 420 + gui_triwave((int)t, 9600, 18), desktop_h / 2 - 220, 260, accent);
    draw_soft_orb(sw / 2 + 260 + gui_triwave((int)t + 600, 11600, 22), desktop_h / 2 - 30, 220, accent2);
    draw_soft_orb(sw / 2 + 410 + gui_triwave((int)t + 1600, 8800, 16), desktop_h / 2 + 190, 170, RGB(58, 102, 162));
    draw_soft_orb(sw / 2 - 60 + gui_triwave((int)t + 2100, 12800, 14), desktop_h / 2 + 220, 150, RGB(36, 52, 82));

    vbe_glass_rect(22, 18, 122, desktop_h - 36, 30, RGB(10, 14, 22), 198);
    vbe_rounded_rect_outline(22, 18, 122, desktop_h - 36, 30, 1, RGB(70, 94, 128));
    vbe_blend_rect(38, 36, 2, desktop_h - 72, accent, 144);

    hero_x = sw - hero_w - 112;
    hero_y = desktop_h / 2 - 188;
    if (hero_x < 180) hero_x = sw / 2 - hero_w / 2;

    vbe_blend_rounded_rect(hero_x + 10, hero_y + 18, hero_w, hero_h, 38, RGB(0, 0, 0), 34);
    vbe_glass_rect(hero_x, hero_y, hero_w, hero_h, 38, RGB(10, 14, 22), 220);
    vbe_blend_rect(hero_x + 1, hero_y + 1, hero_w - 2, 76, RGB(18, 24, 36), 236);
    vbe_rounded_rect_outline(hero_x, hero_y, hero_w, hero_h, 38, 1, RGB(70, 94, 128));
    vbe_gradient_h(hero_x + 36, hero_y + 30, 190, 10, accent, accent2);

    font_draw_string(hero_x + 36, hero_y + 70, "NovaOS Desktop", COLOR_WHITE, COLOR_TRANS, FONT_TITLE);
    font_draw_string(hero_x + 38, hero_y + 116, "GUI repensee : verre fume, chrome sombre et photos dynamiques.", RGB(198, 210, 228), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(hero_x + 38, hero_y + 140, "Wallpaper et lockscreen changent a chaque boot.", RGB(156, 176, 202), COLOR_TRANS, FONT_NORMAL);

    vbe_glass_rect(hero_x + 36, hero_y + 182, 254, 82, 20, RGB(18, 24, 36), 224);
    vbe_rounded_rect_outline(hero_x + 36, hero_y + 182, 254, 82, 20, 1, RGB(70, 94, 128));
    font_draw_string(hero_x + 56, hero_y + 204, "Theme", RGB(130, 150, 178), COLOR_TRANS, FONT_SMALL);
    font_draw_string(hero_x + 56, hero_y + 226, "Dark everywhere", COLOR_WHITE, COLOR_TRANS, FONT_LARGE);
    font_draw_string(hero_x + 56, hero_y + 248, "Taskbar, fenetres, widgets, start menu", RGB(152, 172, 198), COLOR_TRANS, FONT_SMALL);

    vbe_glass_rect(hero_x + 312, hero_y + 182, 270, 82, 20, RGB(18, 24, 36), 224);
    vbe_rounded_rect_outline(hero_x + 312, hero_y + 182, 270, 82, 20, 1, RGB(70, 94, 128));
    font_draw_string(hero_x + 332, hero_y + 204, "Photos", RGB(130, 150, 178), COLOR_TRANS, FONT_SMALL);
    font_draw_string(hero_x + 332, hero_y + 226, "16 uploads integres", COLOR_WHITE, COLOR_TRANS, FONT_LARGE);
    font_draw_string(hero_x + 332, hero_y + 248, "Selection pseudo-aleatoire au demarrage", RGB(152, 172, 198), COLOR_TRANS, FONT_SMALL);
}

static void gui_draw_wallpaper_cached(void) {
    int desktop_h = gui_screen_h() - TASKBAR_H;
    size_t bytes = (size_t)vbe.pitch * (size_t)desktop_h;
    uint32_t now = timer_ms();

    if (!g_wallpaper_cache && bytes > 0) {
        g_wallpaper_cache = (uint8_t*)kmalloc(bytes);
    }

    if (!g_wallpaper_cache) {
        draw_wallpaper();
        return;
    }

    if (!g_wallpaper_cache_valid || now - g_wallpaper_cache_tick >= 250) {
        draw_wallpaper();
        __builtin_memcpy(g_wallpaper_cache, vbe.backbuffer, bytes);
        g_wallpaper_cache_tick = now;
        g_wallpaper_cache_valid = 1;
    } else {
        __builtin_memcpy(vbe.backbuffer, g_wallpaper_cache, bytes);
    }
}

void gui_draw_window_frame(window_t *win) {
    if (!win->visible || win->state == WIN_STATE_MINIMIZED) return;

    int x = win->x, y = win->y, w = win->w, h = win->h;
    int tf = TITLE_BAR_H;
    int btn_w = 28;
    int close_x = x + w - 12 - btn_w;
    int max_x   = close_x - 6 - btn_w;
    int min_x   = max_x   - 6 - btn_w;
    uint32_t pulse = timer_ms();
    color_t accent = gui_accent_primary();
    color_t accent2 = gui_accent_secondary();

    if (win->flags & WIN_SHADOW) {
        vbe_blend_rounded_rect(x + 10, y + 16, w, h, 24, RGB(0,0,0), 28);
        vbe_blend_rounded_rect(x + 18, y + 28, w - 4, h - 6, 22, RGB(0,0,0), 14);
    }

    vbe_glass_rect(x, y, w, h, 24, RGB(14, 18, 28), 214);
    vbe_blend_rounded_rect(x, y, w, h, 24, RGB(14, 18, 28), 204);
    vbe_rounded_rect_outline(x, y, w, h, 24, 1,
                             win->focused ? blend_color(accent, RGB(90,120,160), 136) : RGB(64,78,102));
    vbe_blend_rect(x + 1, y + 1, w - 2, tf + 10, RGB(18, 24, 36), 232);

    if (!(win->flags & WIN_BORDERLESS)) {
        vbe_gradient_v(x + 1, y + 1, w - 2, tf, RGB(18,24,36), RGB(12,16,26));
        vbe_gradient_h(x + 24, y + 6, w - 48, 4,
                       win->focused ? accent : RGB(72,84,102),
                       win->focused ? accent2 : RGB(90,100,120));
        vbe_blend_rect(x + 1, y + 1, w - 2, tf / 2, COLOR_WHITE, 12);
        vbe_gradient_h(x + 12, y + tf - 1, w - 24, 1,
                       win->focused ? RGB(90,132,196) : RGB(64,78,102),
                       win->focused ? RGB(84,170,214) : RGB(64,78,102));

        vbe_circle_fill(x + 18, y + tf / 2, 8, win->focused ? accent : RGB(82,94,112));
        vbe_circle_fill(x + 18, y + tf / 2, 4, RGB(10, 14, 22));
        font_draw_string(x + 34, y + (tf - FONT_H) / 2, win->title,
                         win->focused ? COLOR_WHITE : RGB(188, 198, 214), COLOR_TRANS, FONT_NORMAL);

        {
            int btn_cy = y + tf / 2;
            color_t slot_bg = RGB(26, 34, 46);
            color_t slot_bd = RGB(70, 84, 108);

            vbe_blend_rounded_rect(close_x + 2, btn_cy - 11, 24, 24, 12,
                                   win->close_btn_hover ? RGB(78,32,38) : slot_bg,
                                   win->close_btn_hover ? 255 : 236);
            vbe_rounded_rect_outline(close_x + 2, btn_cy - 11, 24, 24, 12, 1,
                                     win->close_btn_hover ? RGB(255,112,126) : slot_bd);
            vbe_line(close_x + 11, btn_cy - 2, close_x + 19, btn_cy + 6,
                     win->close_btn_hover ? RGB(255,164,176) : RGB(188,198,214));
            vbe_line(close_x + 19, btn_cy - 2, close_x + 11, btn_cy + 6,
                     win->close_btn_hover ? RGB(255,164,176) : RGB(188,198,214));

            if (win->flags & WIN_MAXIMIZABLE) {
                vbe_blend_rounded_rect(max_x + 2, btn_cy - 11, 24, 24, 12,
                                       win->max_btn_hover ? RGB(18,54,90) : slot_bg,
                                       win->max_btn_hover ? 255 : 236);
                vbe_rounded_rect_outline(max_x + 2, btn_cy - 11, 24, 24, 12, 1,
                                         win->max_btn_hover ? RGB(110,196,255) : slot_bd);
                vbe_rect_outline(max_x + 10, btn_cy - 3, 8, 8, 1,
                                 win->max_btn_hover ? RGB(110,196,255) : RGB(188,198,214));
            }

            if (win->flags & WIN_MINIMIZABLE) {
                vbe_blend_rounded_rect(min_x + 2, btn_cy - 11, 24, 24, 12,
                                       win->min_btn_hover ? RGB(88,60,18) : slot_bg,
                                       win->min_btn_hover ? 255 : 236);
                vbe_rounded_rect_outline(min_x + 2, btn_cy - 11, 24, 24, 12, 1,
                                         win->min_btn_hover ? RGB(255,210,118) : slot_bd);
                vbe_line(min_x + 10, btn_cy + 3, min_x + 18, btn_cy + 3,
                         win->min_btn_hover ? RGB(255,210,118) : RGB(188,198,214));
            }
        }

        if (win->focused) {
            int glow = 36 + (gui_triwave((int)pulse, 1800, 8) + 8);
            vbe_blend_rounded_rect(x + 10, y + tf + 12, 8, h - tf - 24, 4, accent, (uint8_t)glow);
        }
    }
}

void gui_draw_window(window_t *win) {
    if (!win->visible || win->state == WIN_STATE_MINIMIZED) return;

    gui_draw_window_frame(win);

    if (win->on_paint) {
        win->on_paint(win);
    }

    for (int i = 0; i < win->widget_count; i++) {
        if (win->widgets[i].visible) {
            gui_draw_widget(&win->widgets[i]);
        }
    }

    win->needs_redraw = 0;
}

void gui_draw_button(widget_t *w) {
    color_t base = !w->enabled ? RGB(54,60,74)
                               : (w->pressed ? RGB(54,96,182)
                                             : (w->hovered ? RGB(72,120,214) : RGB(62,108,198)));
    color_t edge = !w->enabled ? RGB(74,80,94)
                               : (w->hovered ? gui_accent_soft() : RGB(96,126,174));
    color_t fg = COLOR_WHITE;

    vbe_blend_rounded_rect(w->x + 2, w->y + 6, w->w, w->h, 16, RGB(0,0,0), 26);
    vbe_rounded_rect(w->x, w->y, w->w, w->h, 16, base);
    vbe_gradient_v(w->x + 1, w->y + 1, w->w - 2, w->h / 2,
                   blend_color(COLOR_WHITE, base, 68), base);
    vbe_blend_rect(w->x + 1, w->y + 1, w->w - 2, 10, COLOR_WHITE, 18);
    vbe_rounded_rect_outline(w->x, w->y, w->w, w->h, 16, 1, edge);

    {
        int tw = font_string_width(w->text, FONT_NORMAL);
        int tx = w->x + (w->w - tw) / 2;
        int ty = w->y + (w->h - FONT_H) / 2;
        font_draw_string(tx, ty, w->text, fg, COLOR_TRANS, FONT_NORMAL);
    }
}

void gui_draw_label(widget_t *w) {
    if (w->bg != COLOR_TRANS)
        vbe_blend_rounded_rect(w->x, w->y, w->w, w->h, 10, w->bg, 220);
    font_draw_string(w->x, w->y + (w->h - FONT_H)/2, w->text,
                     w->fg ? w->fg : RGB(212, 220, 236), COLOR_TRANS, FONT_NORMAL);
}

void gui_draw_textinput(widget_t *w) {
    color_t border = w->focused ? gui_accent_soft() : RGB(72, 86, 110);
    vbe_glass_rect(w->x, w->y, w->w, w->h, 14, RGB(16, 22, 34), 228);
    vbe_blend_rect(w->x + 1, w->y + 1, w->w - 2, w->h / 2, COLOR_WHITE, 12);
    vbe_rounded_rect_outline(w->x, w->y, w->w, w->h, 14, w->focused ? 2 : 1, border);

    {
        const char *text2 = w->text[0] ? w->text : w->placeholder;
        color_t tc = w->text[0] ? RGB(228, 234, 244) : RGB(126, 144, 172);
        int tx = w->x + 12;
        int ty = w->y + (w->h - FONT_H) / 2;
        font_draw_string(tx, ty, text2, tc, COLOR_TRANS, FONT_NORMAL);

        if (w->focused && (timer_ticks() % 60 < 30)) {
            int cw = font_char_width(FONT_NORMAL);
            int cx = tx + w->cursor_pos * cw;
            vbe_rect(cx, ty, 2, FONT_H, gui_accent_soft());
        }
    }
}

void gui_draw_textarea(widget_t *w) {
    color_t border = w->focused ? gui_accent_soft() : RGB(72, 86, 110);
    vbe_glass_rect(w->x, w->y, w->w, w->h, 16, RGB(16, 22, 34), 228);
    vbe_blend_rect(w->x + 1, w->y + 1, w->w - 2, 28, COLOR_WHITE, 10);
    vbe_rounded_rect_outline(w->x, w->y, w->w, w->h, 16, w->focused ? 2 : 1, border);

    if (w->text[0]) {
        int x = w->x + 10;
        int y = w->y + 10;
        const char *p = w->text;
        while (*p) {
            if (*p == '\n') {
                y += FONT_H + 2;
                x = w->x + 10;
            } else {
                if (y + FONT_H > w->y + w->h - 8) break;
                font_draw_char(x, y, *p, RGB(224, 232, 244), COLOR_TRANS, FONT_NORMAL);
                x += FONT_W;
                if (x + FONT_W > w->x + w->w - 8) {
                    y += FONT_H + 2;
                    x = w->x + 10;
                }
            }
            p++;
        }
        if (w->focused && (timer_ticks() % 60 < 30)) vbe_rect(x, y, 2, FONT_H, gui_accent_soft());
    }
}

void gui_draw_listbox(widget_t *w) {
    vbe_glass_rect(w->x, w->y, w->w, w->h, 16, RGB(16, 22, 34), 220);
    vbe_rounded_rect_outline(w->x, w->y, w->w, w->h, 16, 1, RGB(72, 86, 110));

    {
        int ih = 28;
        for (int i = 0; i < w->list_count; i++) {
            int iy = w->y + 6 + i * ih;
            if (iy + ih > w->y + w->h - 6) break;
            if (i == w->list_selected) {
                vbe_rounded_rect(w->x + 6, iy + 2, w->w - 12, ih - 4, 10, gui_accent_primary());
                vbe_blend_rect(w->x + 7, iy + 3, w->w - 14, 8, COLOR_WHITE, 26);
                font_draw_string(w->x + 12, iy + (ih - FONT_H) / 2, w->list_items[i], COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
            } else {
                if (i % 2 == 0) vbe_blend_rect(w->x + 4, iy + 1, w->w - 8, ih - 2, RGB(22, 28, 42), 160);
                font_draw_string(w->x + 12, iy + (ih - FONT_H) / 2, w->list_items[i], RGB(214, 224, 238), COLOR_TRANS, FONT_NORMAL);
            }
        }
    }
}

void gui_draw_checkbox(widget_t *w) {
    vbe_glass_rect(w->x, w->y, 20, 20, 7, RGB(16, 22, 34), 224);
    vbe_rounded_rect_outline(w->x, w->y, 20, 20, 7, w->checked ? 2 : 1,
                             w->checked ? gui_accent_soft() : RGB(72, 86, 110));
    if (w->checked) {
        vbe_rounded_rect(w->x + 3, w->y + 3, 14, 14, 4, gui_accent_primary());
        vbe_blend_rect(w->x + 4, w->y + 4, 12, 5, COLOR_WHITE, 24);
    }
    font_draw_string(w->x + 28, w->y + 2, w->text, RGB(214, 224, 238), COLOR_TRANS, FONT_NORMAL);
}

void gui_draw_progressbar(widget_t *w) {
    int filled = w->max_value ? (w->w * w->value) / w->max_value : 0;
    vbe_glass_rect(w->x, w->y, w->w, w->h, 10, RGB(16, 22, 34), 220);
    if (filled > 0) {
        vbe_gradient_h(w->x, w->y, filled, w->h, gui_accent_primary(), gui_accent_secondary());
        vbe_blend_rect(w->x, w->y, filled, w->h / 2, COLOR_WHITE, 24);
    }
    vbe_rounded_rect_outline(w->x, w->y, w->w, w->h, 10, 1, RGB(72, 86, 110));
    {
        char pct[8];
        int tw;
        gui_itoa(w->value, pct);
        tw = font_string_width(pct, FONT_SMALL);
        font_draw_string(w->x + (w->w - tw) / 2, w->y + (w->h - FONT_SM_H) / 2,
                         pct, RGB(220, 228, 240), COLOR_TRANS, FONT_SMALL);
    }
}

void gui_draw_slider(widget_t *w) {
    int range = (w->max_value - w->min_value);
    int knob_x = range ? ((w->value - w->min_value) * (w->w - 18)) / range : 0;
    int track_y = w->y + w->h / 2 - 4;
    vbe_glass_rect(w->x, track_y, w->w, 8, 4, RGB(20, 28, 42), 196);
    vbe_rounded_rect(w->x, track_y, knob_x + 9, 8, 4, gui_accent_primary());
    vbe_blend_rect(w->x, track_y, knob_x + 9, 4, COLOR_WHITE, 26);
    vbe_circle_fill(knob_x + w->x + 9, w->y + w->h / 2, 11, RGB(220, 228, 240));
    vbe_circle_fill(knob_x + w->x + 9, w->y + w->h / 2, 7, gui_accent_primary());
}

void gui_draw_widget(widget_t *w) {
    if (!w->visible) return;
    switch(w->type) {
        case WIDGET_BUTTON:      gui_draw_button(w);      break;
        case WIDGET_LABEL:       gui_draw_label(w);       break;
        case WIDGET_TEXTINPUT:   gui_draw_textinput(w);   break;
        case WIDGET_TEXTAREA:    gui_draw_textarea(w);    break;
        case WIDGET_LISTBOX:     gui_draw_listbox(w);     break;
        case WIDGET_CHECKBOX:    gui_draw_checkbox(w);    break;
        case WIDGET_PROGRESSBAR: gui_draw_progressbar(w); break;
        case WIDGET_SLIDER:      gui_draw_slider(w);      break;
        default: break;
    }
}

void gui_draw_taskbar(void) {
    int ty = gui_screen_h() - TASKBAR_H;
    int sw = gui_screen_w();
    color_t accent = gui_accent_primary();
    color_t accent2 = gui_accent_secondary();

    vbe_blend_rect(0, ty, sw, TASKBAR_H, RGB(8, 12, 18), 218);
    vbe_gradient_h(0, ty, sw, 1, accent, accent2);

    {
        int gx = taskbar_group_left();
        int gw = taskbar_group_w();
        int sy = ty + 7;
        vbe_glass_rect(gx - 12, sy, gw + 24, 42, 22, RGB(12, 18, 28), 214);
        vbe_blend_rect(gx - 10, sy + 1, gw + 20, 10, COLOR_WHITE, 10);
        vbe_rounded_rect_outline(gx - 12, sy, gw + 24, 42, 22, 1, RGB(70, 94, 128));
    }

    {
        int sx = taskbar_start_left();
        color_t sbg = gui.taskbar_start_hovered ? accent : RGB(18, 24, 36);
        vbe_blend_rounded_rect(sx, ty + 10, 48, 36, 16, sbg, 244);
        vbe_rounded_rect_outline(sx, ty + 10, 48, 36, 16, 1,
                                 gui.taskbar_start_hovered ? gui_accent_soft() : RGB(70, 94, 128));
        {
            const ui_bitmap_t *start_bmp = ui_bitmap_start_button();
            if (start_bmp) ui_draw_bitmap_scaled(start_bmp, sx + 15, ty + 16, 18, 18);
            else draw_shell_logo(sx + 12, ty + 18, 18,
                                 COLOR_WHITE,
                                 gui.taskbar_start_hovered ? blend_color(COLOR_WHITE, accent2, 88) : RGB(132,210,255));
        }
    }

    {
        int bx = taskbar_windows_left();
        int shown = 0;
        for (int i = 0; i < gui.window_count; i++) {
            window_t *win = &gui.windows[i];
            int focused;
            const ui_bitmap_t *tbmp;
            if (!win->visible) continue;
            if (shown >= 6) break;
            focused = (i == gui.focused_window);
            vbe_glass_rect(bx, ty + 10, 46, 36, 14,
                           focused ? blend_color(RGB(18,24,36), accent, 42) : RGB(18,24,36),
                           focused ? 228 : 208);
            vbe_rounded_rect_outline(bx, ty + 10, 46, 36, 14, 1,
                                     focused ? gui_accent_soft() : RGB(70, 94, 128));
            tbmp = ui_bitmap_for_window_title(win->title);
            if (tbmp) {
                ui_draw_bitmap_scaled(tbmp, bx + 10, ty + 14, 26, 26);
            } else {
                char sym[2];
                sym[0] = win->title[0] ? win->title[0] : '?';
                sym[1] = 0;
                font_draw_string(bx + 18, ty + 19, sym, focused ? COLOR_WHITE : RGB(188,198,214), COLOR_TRANS, FONT_NORMAL);
            }
            vbe_blend_rounded_rect(bx + 11, ty + 41, 24, 3, 2,
                                   focused ? accent : RGB(88, 104, 128), 255);
            bx += 54;
            shown++;
        }
    }

    {
        int tray_w = taskbar_tray_w();
        int tray_x = taskbar_tray_x();
        color_t net_c = net_eth0.connected ? RGB(78,214,148) : RGB(244,112,112);
        char time_str[8];
        vbe_glass_rect(tray_x, ty + 8, tray_w, 40, 18, RGB(12, 18, 28), 214);
        vbe_rounded_rect_outline(tray_x, ty + 8, tray_w, 40, 18, 1, RGB(70, 94, 128));
        vbe_circle_fill(tray_x + 18, ty + 27, 5, net_c);
        time_str[0] = '0' + (gui.hour / 10);
        time_str[1] = '0' + (gui.hour % 10);
        time_str[2] = ':';
        time_str[3] = '0' + (gui.min / 10);
        time_str[4] = '0' + (gui.min % 10);
        time_str[5] = 0;
        font_draw_string(tray_x + 34, ty + 15, time_str, COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
        font_draw_string(tray_x + 94, ty + 18, net_eth0.connected ? "En ligne" : "Hors ligne", RGB(154, 174, 198), COLOR_TRANS, FONT_SMALL);
    }

    if (gui.notif_text[0] && timer_ms() - gui.notif_time < 2600) {
        int nw = font_string_width(gui.notif_text, FONT_NORMAL) + 54;
        int nx = sw - nw - 24;
        int ny = ty - 62;
        vbe_glass_rect(nx, ny, nw, 46, 18, RGB(12, 18, 28), 214);
        vbe_rounded_rect_outline(nx, ny, nw, 46, 18, 1, RGB(70, 94, 128));
        font_draw_string(nx + 18, ny + 15, gui.notif_text, RGB(220, 228, 240), COLOR_TRANS, FONT_NORMAL);
    }
}

static const uint8_t cursor_shape[18][18] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,1,1,0,0,0,0,0,0,0},
    {1,2,2,2,1,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,1,2,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,1,2,2,2,1,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,1,2,2,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,2,2,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,2,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0},
};

static uint32_t cursor_save[18][18];
static int cursor_saved_x = -1, cursor_saved_y = -1;

static void gui_swap_cursor_regions(int old_x, int old_y, int new_x, int new_y) {
    if (old_x >= 0 && old_y >= 0) vbe_swap_rect(old_x, old_y, 20, 20);
    if (new_x >= 0 && new_y >= 0 && (new_x != old_x || new_y != old_y))
        vbe_swap_rect(new_x, new_y, 20, 20);
}

void gui_draw_cursor(void) {
    if (cursor_saved_x >= 0) {
        for (int cy = 0; cy < 18; cy++)
            for (int cx = 0; cx < 18; cx++)
                vbe_put_pixel(cursor_saved_x+cx, cursor_saved_y+cy, cursor_save[cy][cx]);
    }

    int mx = gui.cursor_x, my = gui.cursor_y;
    cursor_saved_x = mx; cursor_saved_y = my;

    for (int cy = 0; cy < 18; cy++) {
        for (int cx = 0; cx < 18; cx++) {
            cursor_save[cy][cx] = vbe_get_pixel(mx+cx, my+cy);
            if (cursor_shape[cy][cx]) {
                color_t bg = vbe_get_pixel(mx+cx+1, my+cy+1);
                vbe_put_pixel(mx+cx+1, my+cy+1, blend_color(COLOR_BLACK, bg, 85));
            }
        }
    }

    for (int cy = 0; cy < 18; cy++) {
        for (int cx = 0; cx < 18; cx++) {
            uint8_t v = cursor_shape[cy][cx];
            if (v == 1) vbe_put_pixel(mx+cx, my+cy, RGB(72, 92, 128));
            else if (v == 2) vbe_put_pixel(mx+cx, my+cy, RGB(255,255,255));
        }
    }
}

typedef struct {
    char name[64];
    void (*fn)(void);
    int launch_mode;
    char target[256];
} menuitem_t;

static int start_menu_open = 0;
static menuitem_t start_items[32];
static int start_item_count = 0;

static void gui_launch_link_target(const char *target) {
    if (!target || !target[0]) return;
    app_browser_open_url(target);
}

static void gui_open_icon(desktop_icon_t *ico) {
    if (!ico) return;
    if (ico->launch_mode == 1 && ico->target[0]) gui_launch_link_target(ico->target);
    else if (ico->on_open) ico->on_open();
}

static int gui_start_has_entry(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < start_item_count; i++) {
        if (str_equal(start_items[i].name, name)) return 1;
    }
    return 0;
}

static void gui_add_start_builtin(const char *name, void (*fn)(void)) {
    menuitem_t *it;
    if (start_item_count >= 32 || gui_start_has_entry(name)) return;
    it = &start_items[start_item_count++];
    __builtin_memset(it, 0, sizeof(*it));
    gui_string_copy(it->name, name, 64);
    it->fn = fn;
}

static void gui_add_start_link(const char *name, const char *target) {
    menuitem_t *it;
    if (start_item_count >= 32 || gui_start_has_entry(name)) return;
    it = &start_items[start_item_count++];
    __builtin_memset(it, 0, sizeof(*it));
    gui_string_copy(it->name, name, 64);
    it->launch_mode = 1;
    gui_string_copy(it->target, target, 256);
}

static int gui_point_in_desktop_icon(int mx, int my) {
    for (int i = 0; i < gui.icon_count; i++) {
        desktop_icon_t *ico = &gui.icons[i];
        if (mx >= ico->x && mx < ico->x + ICON_SIZE &&
            my >= ico->y && my < ico->y + ICON_SIZE + 20) return 1;
    }
    return 0;
}

static int gui_point_in_visible_window(int mx, int my) {
    for (int i = gui.window_count - 1; i >= 0; i--) {
        window_t *win = &gui.windows[i];
        if (!win->visible || win->state == WIN_STATE_MINIMIZED) continue;
        if (mx >= win->x && mx < win->x + win->w &&
            my >= win->y && my < win->y + win->h) return 1;
    }
    return 0;
}

void gui_add_desktop_icon(int x, int y, const char *name, void (*on_open)(void)) {
    if (gui.icon_count >= 32) return;
    desktop_icon_t *ico = &gui.icons[gui.icon_count++];
    __builtin_memset(ico, 0, sizeof(*ico));
    ico->x = x; ico->y = y;
    gui_string_copy(ico->name, name, 64);
    ico->on_open = on_open;
    ico->hovered = 0;
    ico->selected = 0;
    ico->icon = NULL;
}

void gui_add_desktop_icon_link(int x, int y, const char *name, const char *target) {
    if (gui.icon_count >= 32) return;
    desktop_icon_t *ico = &gui.icons[gui.icon_count++];
    __builtin_memset(ico, 0, sizeof(*ico));
    ico->x = x; ico->y = y;
    gui_string_copy(ico->name, name, 64);
    ico->launch_mode = 1;
    gui_string_copy(ico->target, target, 256);
    ico->hovered = 0;
    ico->selected = 0;
    ico->icon = NULL;
}

static void gui_desktop_icon_slot(int index, int *x, int *y) {
    int base_x = 34;
    int base_y = 38;
    int step_x = 110;
    int step_y = ICON_SIZE + 38;
    int desktop_h = gui_screen_h() - TASKBAR_H;
    int usable_h = desktop_h - base_y - (ICON_SIZE + 20);
    int rows = 1;

    if (usable_h > 0) rows += usable_h / step_y;
    if (rows < 1) rows = 1;

    *x = base_x + (index / rows) * step_x;
    *y = base_y + (index % rows) * step_y;
}

static void gui_add_desktop_icon_auto(const char *name, void (*on_open)(void)) {
    int x, y;
    gui_desktop_icon_slot(gui.icon_count, &x, &y);
    gui_add_desktop_icon(x, y, name, on_open);
}

static void gui_add_desktop_icon_link_auto(const char *name, const char *target) {
    int x, y;
    gui_desktop_icon_slot(gui.icon_count, &x, &y);
    gui_add_desktop_icon_link(x, y, name, target);
}

static void gui_build_builtin_shortcuts(void) {
    gui.icon_count = 0;
    start_item_count = 0;

    gui_add_desktop_icon_auto("Fichiers", app_filemanager_open);
    gui_add_desktop_icon_auto("Navigateur", app_browser_open);
    gui_add_desktop_icon_auto("Centre Système", app_system_monitor_open);
    gui_add_desktop_icon_auto("Tableau de bord", app_nova_hub_open);
    gui_add_desktop_icon_auto("Commandes", app_symera_open);

    gui_add_start_builtin("Fichiers", app_filemanager_open);
    gui_add_start_builtin("Navigateur", app_browser_open);
    gui_add_start_builtin("Centre Système", app_system_monitor_open);
    gui_add_start_builtin("Tableau de bord", app_nova_hub_open);
    gui_add_start_builtin("Commandes", app_symera_open);
    gui_add_start_builtin("Notes", app_quick_notes_open);
    gui_add_start_builtin("Parametres", app_settings_open);
    gui_add_start_builtin("Terminal", app_terminal_open);
    gui_add_start_builtin("Editeur", app_editor_open);
    gui_add_start_builtin("Calculatrice", app_calculator_open);
    gui_add_start_builtin("Userspace", app_userspace_open);
    gui_add_start_builtin("Horloge", app_clock_open);
    gui_add_start_builtin("Installer", app_installer_open);
    gui_add_start_builtin("Tutoriel", app_tutorial_open);
    gui_add_start_link("Applications", "nova://store");
    gui_add_start_builtin("A propos", app_about_open);
}

void gui_refresh_shortcuts(void) {
    nova_shortcut_t sc[16];
    int count;
    gui_build_builtin_shortcuts();

    count = nova_pkg_load_shortcuts("/home/user/Desktop", sc, 16);
    for (int i = 0; i < count; i++) {
        gui_add_desktop_icon_link_auto(sc[i].name, sc[i].launch_target);
    }

    count = nova_pkg_load_shortcuts("/system/menu", sc, 16);
    for (int i = 0; i < count; i++) {
        gui_add_start_link(sc[i].name, sc[i].launch_target);
    }
}

static void draw_app_icon(int x, int y, int w, int h, color_t c1, color_t c2, const char *name, const char *sym) {
    int bob = gui_triwave((int)timer_ms() + x * 9 + y * 7, 2400, 1);
    int use_folder_shape = 0;
    y += bob;
    const ui_bitmap_t *bmp = use_folder_shape ? NULL : ui_bitmap_for_name(name);
    vbe_blend_rounded_rect(x - 2, y + 6, w + 4, h + 6, 20, RGB(120,146,188), 10);
    if (use_folder_shape) {
        vbe_blend_rounded_rect(x + 4, y + 12, w - 8, h - 16, 16, RGB(255,198,96), 244);
        vbe_blend_rounded_rect(x + 12, y + 6, w / 2, 18, 8, RGB(255,222,146), 240);
        vbe_blend_rect(x + 8, y + 20, w - 16, 10, COLOR_WHITE, 24);
        vbe_rounded_rect_outline(x + 4, y + 12, w - 8, h - 16, 16, 1, RGB(214,166,72));
    } else if (bmp) {
        vbe_glass_rect(x + 2, y + 6, w - 4, h - 2, 18, RGB(255,255,255), 86);
        ui_draw_bitmap_scaled(bmp, x - 2, y - 2, w + 4, h + 4);
    } else {
        vbe_glass_rect(x, y, w, h, 18, RGB(255,255,255), 114);
        vbe_gradient_v(x + 1, y + 1, w - 2, h - 2, blend_color(COLOR_WHITE, c1, 96), c1);
        vbe_blend_rect(x + 1, y + 1, w - 2, h / 2, COLOR_WHITE, 34);
        vbe_rounded_rect_outline(x, y, w, h, 18, 1, c2);
        if (sym && sym[0]) {
            int tx = x + (w - font_char_width(FONT_LARGE)) / 2;
            int ty = y + (h - font_char_height(FONT_LARGE)) / 2;
            font_draw_string(tx, ty, sym, RGB(248,250,255), COLOR_TRANS, FONT_LARGE);
        }
    }
}

void gui_draw_desktop_icon(desktop_icon_t *ico) {
    int x = ico->x, y = ico->y;
    int iw = ICON_SIZE, ih = ICON_SIZE;
    const char *sym = ui_symbol_for_name(ico->name);
    color_t c1 = RGB(96,132,255), c2 = RGB(180,206,255);

    if (str_equal(ico->name, "Terminal"))      { c1 = RGB(68,86,116);  c2 = RGB(126,236,188); }
    else if (str_equal(ico->name, "Editeur"))  { c1 = RGB(108,148,255); c2 = RGB(184,214,255); }
    else if (str_equal(ico->name, "Fichiers")) { c1 = RGB(255,194,96);  c2 = RGB(255,226,154); }
    else if (str_equal(ico->name, "Calculatrice")) { c1 = RGB(82,208,170); c2 = RGB(176,248,220); }
    else if (str_equal(ico->name, "Navigateur")) { c1 = RGB(94,142,255); c2 = RGB(190,214,255); }
    else if (str_equal(ico->name, "Parametres")) { c1 = RGB(126,138,164); c2 = RGB(208,216,232); }
    else if (str_equal(ico->name, "Centre Système")) { c1 = RGB(72,196,156); c2 = RGB(170,246,224); }
    else if (str_equal(ico->name, "Userspace"))  { c1 = RGB(88,122,255); c2 = RGB(160,228,255); }
    else if (str_equal(ico->name, "Horloge"))    { c1 = RGB(212,118,232); c2 = RGB(242,196,255); }
    else if (str_equal(ico->name, "Installer"))  { c1 = RGB(255,154,86);  c2 = RGB(255,214,156); }
    else if (str_equal(ico->name, "A propos"))   { c1 = RGB(86,206,236);  c2 = RGB(176,236,255); }
    else if (str_equal(ico->name, "Tableau de bord"))   { c1 = RGB(110,132,255); c2 = RGB(202,214,255); }
    else if (str_equal(ico->name, "Notes")){ c1 = RGB(255,188,96);  c2 = RGB(255,226,170); }
    else if (str_equal(ico->name, "Commandes"))     { c1 = RGB(82,132,255);  c2 = RGB(134,236,220); }

    if (ico->hovered || ico->selected) {
        vbe_glass_rect(x - 12, y - 12, iw + 24, ih + 48, 20,
                       ico->selected ? RGB(18,24,36) : RGB(14,20,30),
                       ico->selected ? 120 : 88);
        vbe_rounded_rect_outline(x - 12, y - 12, iw + 24, ih + 48, 20, 1,
                                 ico->selected ? RGB(118,172,255) : RGB(72,94,128));
    }

    draw_app_icon(x, y, iw, ih, c1, c2, ico->name, sym);

    int tw = font_string_width(ico->name, FONT_SMALL);
    int tx = x + (iw - tw) / 2;
    vbe_blend_rounded_rect(tx - 10, y + ih + 10, tw + 20, FONT_SM_H + 10, 11,
                           RGB(16,22,34), ico->selected ? 184 : 150);
    font_draw_string(tx, y + ih + 14, ico->name, RGB(228,236,246), COLOR_TRANS, FONT_SMALL);
}

static void gui_draw_start_menu(void) {
    if (!start_menu_open) return;

    int mx, my, mw, mh;
    color_t accent = gui_accent_primary();
    color_t accent2 = gui_accent_secondary();
    user_t *cur = users_get_current();
    const char *uname = (cur && cur->fullname[0]) ? cur->fullname : (cur && cur->username[0] ? cur->username : "user");

    start_menu_bounds(&mx, &my, &mw, &mh);

    vbe_blend_rounded_rect(mx + 10, my + 18, mw, mh, 30, RGB(0,0,0), 34);
    vbe_glass_rect(mx, my, mw, mh, 30, RGB(12, 18, 28), 220);
    vbe_blend_rect(mx + 1, my + 1, mw - 2, 70, RGB(18, 24, 36), 236);
    vbe_rounded_rect_outline(mx, my, mw, mh, 30, 1, RGB(70, 94, 128));
    vbe_gradient_h(mx + 28, my + 24, 138, 12, accent, accent2);

    font_draw_string(mx + 34, my + 28, "NovaOS", COLOR_WHITE, COLOR_TRANS, FONT_LARGE);
    font_draw_string(mx + 34, my + 56, "Acces rapide · applications et systeme local", RGB(162, 180, 208), COLOR_TRANS, FONT_SMALL);

    vbe_glass_rect(mx + 30, my + 82, mw - 60, 44, 16, RGB(18, 24, 36), 228);
    vbe_rounded_rect_outline(mx + 30, my + 82, mw - 60, 44, 16, 1, RGB(70, 94, 128));
    font_draw_string(mx + 48, my + 95, "Recherche locale : apps, fichiers, notes, reglages", RGB(138, 156, 184), COLOR_TRANS, FONT_SMALL);

    {
        int grid_x = mx + 34;
        int grid_y = my + 142;
        for (int i = 0; i < start_item_count; i++) {
            int col = i % 5;
            int row = i / 5;
            int tx = grid_x + col * 110;
            int ty = grid_y + row * 120;
            int hovered = (gui.cursor_x >= tx && gui.cursor_x < tx + 92 && gui.cursor_y >= ty && gui.cursor_y < ty + 94);
            vbe_glass_rect(tx, ty, 92, 94, 18,
                           hovered ? blend_color(RGB(18,24,36), accent, 34) : RGB(18,24,36),
                           hovered ? 228 : 214);
            vbe_rounded_rect_outline(tx, ty, 92, 94, 18, 1,
                                     hovered ? gui_accent_soft() : RGB(70, 94, 128));
            draw_app_icon(tx + 21, ty + 14, 50, 50, accent, blend_color(COLOR_WHITE, accent2, 96), start_items[i].name, ui_symbol_for_name(start_items[i].name));
            {
                int tw = font_string_width(start_items[i].name, FONT_SMALL);
                font_draw_string(tx + (92 - tw) / 2, ty + 72, start_items[i].name, RGB(220, 228, 240), COLOR_TRANS, FONT_SMALL);
            }
        }
    }

    vbe_glass_rect(mx + 24, my + mh - 62, mw - 48, 36, 16, RGB(18, 24, 36), 228);
    vbe_rounded_rect_outline(mx + 24, my + mh - 62, mw - 48, 36, 16, 1, RGB(70, 94, 128));
    font_draw_string(mx + 40, my + mh - 49, uname, COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
    font_draw_string(mx + mw - 146, my + mh - 49, "Session securisee · Verrouiller", RGB(154, 174, 198), COLOR_TRANS, FONT_SMALL);
}

void gui_init(void) {
    __builtin_memset(&gui, 0, sizeof(gui_state_t));
    g_session_onboarded = 0;
    gui.desktop_bg  = RGB(10,14,22);
    gui.focused_window = -1;
    gui.cursor_x = gui_screen_w()/2;
    gui.cursor_y = gui_screen_h()/2;

    users_init();

    k_memset(&g_lock, 0, sizeof(g_lock));
    g_lock.selected_user = 1;
    if (user_sys.user_count > 1) {
        k_memcpy(g_lock.username_input, user_sys.users[1].username, 64);
        g_lock.username_len = (int)k_strlen(user_sys.users[1].username);
    } else {
        k_memcpy(g_lock.username_input, "user", 5);
        g_lock.username_len = 4;
    }
    g_lockscreen_active = 1;

    gui_preferences_load();
    gui_select_boot_visuals();
    gui_refresh_shortcuts();
    g_full_redraw_needed = 1;
    g_cursor_redraw_needed = 1;
    g_wallpaper_cache_valid = 0;

    mouse_set_handler(gui_handle_mouse);
    keyboard_set_handler(gui_handle_key);
}

window_t* gui_create_window(int x, int y, int w, int h, const char *title, uint32_t flags) {
    if (gui.window_count >= MAX_WINDOWS) return NULL;

    if (x == -1) x = (gui_screen_w() - w) / 2;
    if (y == -1) y = (gui_screen_h() - TASKBAR_H - h) / 2;
    if (w > gui_screen_w()) w = gui_screen_w();
    if (h > gui_screen_h() - TASKBAR_H) h = gui_screen_h() - TASKBAR_H;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > gui_screen_w()) x = gui_screen_w() - w;
    if (y + h > gui_screen_h() - TASKBAR_H) y = gui_screen_h() - TASKBAR_H - h;

    window_t *win = &gui.windows[gui.window_count++];
    __builtin_memset(win, 0, sizeof(window_t));

    win->x = 0; win->y = 0;
    win->w = 0; win->h = 0;
    win->min_w = 240; win->min_h = 170;
    gui_string_copy(win->title, title, 128);
    win->flags    = flags;
    win->state    = WIN_STATE_NORMAL;
    win->visible  = 1;
    win->focused  = 1;
    win->bg_color = RGB(16,22,34);
    win->title_color = RGB(16,22,34);
    win->title_text_color = RGB(232,238,246);
    win->zorder   = gui.next_zorder++;
    win->id       = gui.next_id++;
    win->needs_redraw = 1;
    gui_apply_window_rect(win, x, y, w, h);

    if (gui.focused_window >= 0)
        gui.windows[gui.focused_window].focused = 0;
    gui.focused_window = gui.window_count - 1;

    gui_request_full_redraw();
    return win;
}

void gui_destroy_window(window_t *win) {

    for (int i = 0; i < gui.window_count; i++) {
        if (&gui.windows[i] == win) {
            for (int j = i; j < gui.window_count-1; j++)
                gui.windows[j] = gui.windows[j+1];
            gui.window_count--;
            if (gui.focused_window >= gui.window_count)
                gui.focused_window = gui.window_count - 1;
            gui_request_full_redraw();
            return;
        }
    }
}

void gui_close_window(window_t *win) { gui_destroy_window(win); }

void gui_minimize_window(window_t *win) {
    gui_request_full_redraw();
    win->state = WIN_STATE_MINIMIZED;
    win->focused = 0;
    if (gui.focused_window >= 0 && &gui.windows[gui.focused_window] == win) {
        gui.focused_window = -1;
        for (int i = gui.window_count-1; i >= 0; i--) {
            if (gui.windows[i].visible && gui.windows[i].state != WIN_STATE_MINIMIZED) {
                gui.focused_window = i;
                gui.windows[i].focused = 1;
                break;
            }
        }
    }
}

void gui_maximize_window(window_t *win) {
    gui_request_full_redraw();
    if (win->state == WIN_STATE_MAXIMIZED) {
        gui_restore_window(win); return;
    }
    win->save_x = win->x; win->save_y = win->y;
    win->save_w = win->w; win->save_h = win->h;
    gui_apply_window_rect(win, 0, 0, gui_screen_w(), gui_screen_h() - TASKBAR_H);
    win->state = WIN_STATE_MAXIMIZED;
}

void gui_restore_window(window_t *win) {
    gui_request_full_redraw();
    gui_apply_window_rect(win, win->save_x, win->save_y, win->save_w, win->save_h);
    win->state = WIN_STATE_NORMAL;
}

static void gui_apply_window_rect(window_t *win, int nx, int ny, int nw, int nh) {
    int max_w = gui_screen_w();
    int max_h = gui_screen_h() - TASKBAR_H;
    if (nw < win->min_w) nw = win->min_w;
    if (nh < win->min_h) nh = win->min_h;
    if (nw > max_w) nw = max_w;
    if (nh > max_h) nh = max_h;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx + nw > max_w) nx = max_w - nw;
    if (ny + nh > max_h) ny = max_h - nh;

    int dx = nx - win->x;
    int dy = ny - win->y;
    for (int j = 0; j < win->widget_count; j++) {
        win->widgets[j].x += dx;
        win->widgets[j].y += dy;
        if (j == 0 && win->widgets[j].w == win->w && win->widgets[j].h == win->h - TITLE_BAR_H) {
            win->widgets[j].w = nw;
            win->widgets[j].h = nh - TITLE_BAR_H;
        }
    }
    win->x = nx;
    win->y = ny;
    win->w = nw;
    win->h = nh;
}

void gui_focus_window(window_t *win) {
    gui_request_full_redraw();
    if (gui.focused_window >= 0)
        gui.windows[gui.focused_window].focused = 0;
    for (int i = 0; i < gui.window_count; i++) {
        if (&gui.windows[i] == win) {
            gui.focused_window = i;
            win->focused = 1;
            win->zorder = gui.next_zorder++;
            if (win->state == WIN_STATE_MINIMIZED) win->state = WIN_STATE_NORMAL;
            return;
        }
    }
}

void gui_show_window(window_t *win) { win->visible = 1; gui_request_full_redraw(); }
void gui_hide_window(window_t *win) { win->visible = 0; gui_request_full_redraw(); }

widget_t* gui_add_widget(window_t *win, widget_type_t type, int x, int y, int w, int h, const char *text) {
    if (win->widget_count >= MAX_WIDGETS) return NULL;
    widget_t *wg = &win->widgets[win->widget_count++];
    __builtin_memset(wg, 0, sizeof(widget_t));

    wg->x = win->x + x;
    wg->y = win->y + TITLE_BAR_H + y;
    wg->w = w; wg->h = h;
    wg->type = type;
    if (text) gui_string_copy(wg->text, text, 256);
    wg->visible  = 1;
    wg->enabled  = 1;
    wg->bg       = COLOR_TRANS;
    wg->fg       = THEME_TEXT_DRK;
    wg->max_value = 100;
    wg->window   = win;
    return wg;
}

widget_t* gui_add_button(window_t *win, int x, int y, int w, int h, const char *text, event_handler_t cb) {
    widget_t *wg = gui_add_widget(win, WIDGET_BUTTON, x, y, w, h, text);
    if (wg) wg->on_click = cb;
    return wg;
}

widget_t* gui_add_label(window_t *win, int x, int y, int w, int h, const char *text) {
    return gui_add_widget(win, WIDGET_LABEL, x, y, w, h, text);
}

widget_t* gui_add_textinput(window_t *win, int x, int y, int w, int h, const char *placeholder) {
    widget_t *wg = gui_add_widget(win, WIDGET_TEXTINPUT, x, y, w, h, "");
    if (wg && placeholder) gui_string_copy(wg->placeholder, placeholder, 128);
    return wg;
}

widget_t* gui_add_textarea(window_t *win, int x, int y, int w, int h) {
    return gui_add_widget(win, WIDGET_TEXTAREA, x, y, w, h, "");
}

widget_t* gui_add_listbox(window_t *win, int x, int y, int w, int h) {
    widget_t *wg = gui_add_widget(win, WIDGET_LISTBOX, x, y, w, h, "");
    if (wg) {
        wg->list_items = (char**)kmalloc(sizeof(char*) * 256);
        wg->list_count = 0;
        wg->list_selected = -1;
    }
    return wg;
}

widget_t* gui_add_checkbox(window_t *win, int x, int y, const char *text) {
    return gui_add_widget(win, WIDGET_CHECKBOX, x, y, 200, 20, text);
}

widget_t* gui_add_progressbar(window_t *win, int x, int y, int w, int h) {
    return gui_add_widget(win, WIDGET_PROGRESSBAR, x, y, w, h, "");
}

widget_t* gui_add_slider(window_t *win, int x, int y, int w, int h, int mn, int mx, int val) {
    widget_t *wg = gui_add_widget(win, WIDGET_SLIDER, x, y, w, h, "");
    if (wg) { wg->min_value=mn; wg->max_value=mx; wg->value=val; }
    return wg;
}

void gui_dispatch_event(window_t *win, gui_event_t *evt) {
    int visual_state_changed = 0;
    for (int i = 0; i < win->widget_count; i++) {
        widget_t *wg = &win->widgets[i];
        if (!wg->visible || !wg->enabled) continue;
        int hit_x = evt->x;
        int hit_y = evt->y;
        if (evt->type == EVT_MOUSEMOVE || evt->type == EVT_MOUSEDOWN || evt->type == EVT_MOUSEUP ||
            evt->type == EVT_CLICK || evt->type == EVT_RCLICK || evt->type == EVT_SCROLL ||
            evt->type == EVT_HOVER || evt->type == EVT_LEAVE) {
            hit_x -= (win->x + 2);
            hit_y -= (win->y + TITLE_BAR_H);
        }
        int in_widget = (hit_x >= wg->x && hit_x < wg->x+wg->w &&
                         hit_y >= wg->y && hit_y < wg->y+wg->h);

        if (evt->type == EVT_MOUSEMOVE) {
            int was_hovered = wg->hovered;
            wg->hovered = in_widget;
            if (was_hovered != wg->hovered) visual_state_changed = 1;
            if (!was_hovered && in_widget && wg->on_hover) {
                gui_event_t hevt = *evt; hevt.type = EVT_HOVER;
                wg->on_hover(wg, &hevt);
            }
        } else if (evt->type == EVT_MOUSEDOWN && in_widget) {
            if (wg->type == WIDGET_TEXTINPUT || wg->type == WIDGET_TEXTAREA) {

                for (int j = 0; j < win->widget_count; j++)
                    win->widgets[j].focused = 0;
                wg->focused = 1;
                visual_state_changed = 1;
            }
            if (!wg->pressed) visual_state_changed = 1;
            wg->pressed = 1;
        } else if (evt->type == EVT_MOUSEUP) {
            int was_pressed = wg->pressed;
            if (wg->pressed && in_widget) {
                wg->pressed = 0;
                visual_state_changed = 1;
                if (wg->type == WIDGET_CHECKBOX) {
                    wg->checked ^= 1;
                    if (wg->on_change) wg->on_change(wg, evt);
                } else if (wg->on_click) {
                    gui_event_t cevt = *evt; cevt.type = EVT_CLICK;
                    wg->on_click(wg, &cevt);
                }
            }
            if (was_pressed && wg->pressed) visual_state_changed = 1;
            wg->pressed = 0;
        } else if (evt->type == EVT_KEYDOWN && wg->focused) {

            if (wg->type == WIDGET_TEXTINPUT) {
                key_event_t *k = &evt->key;
                int len = gui_string_len(wg->text);
                if (k->ascii == '\b') {
                    if (len > 0) {
                        wg->text[len-1] = 0;
                        if (wg->cursor_pos > 0) wg->cursor_pos--;
                    }
                } else if (k->ascii == '\n' || k->ascii == '\r') {
                    if (wg->on_change) wg->on_change(wg, evt);
                } else if (k->ascii >= 32 && k->ascii < 127 && len < 255) {
                    wg->text[len] = k->ascii;
                    wg->text[len+1] = 0;
                    wg->cursor_pos++;
                }
            } else if (wg->type == WIDGET_TEXTAREA) {
                key_event_t *k = &evt->key;
                int len = gui_string_len(wg->text);
                if (k->ascii == '\b') {
                    if (len > 0) wg->text[len-1] = 0;
                } else if (k->ascii >= 32 || k->ascii == '\n' || k->ascii == '\t') {
                    if (len < 8191) {
                        wg->text[len] = k->ascii;
                        wg->text[len+1] = 0;
                    }
                }
            }
            if (wg->on_keydown) wg->on_keydown(wg, evt);
        }
    }
    if (visual_state_changed) gui_request_full_redraw();
}

static int last_click_time = 0;
static int last_click_x = 0, last_click_y = 0;

void gui_handle_mouse(mouse_state_t *state) {
    int old_cursor_x = gui.cursor_x;
    int old_cursor_y = gui.cursor_y;
    uint8_t old_left = gui.cursor_down;
    uint8_t old_right = gui.cursor_right_down;
    int force_full_redraw = g_lockscreen_active;

    gui.cursor_x = state->x;
    gui.cursor_y = state->y;

    int mx = state->x, my = state->y;
    uint8_t btn = state->buttons;
    uint8_t prev_btn = gui.cursor_prev_down;
    gui.cursor_down = (btn & 1);
    gui.cursor_right_down = (btn >> 1) & 1;

    int clicked       = (gui.cursor_down && !prev_btn);
    int released      = (!gui.cursor_down && prev_btn);
    int right_clicked = (gui.cursor_right_down && !gui.cursor_prev_right_down);

    gui.cursor_prev_down       = gui.cursor_down;
    gui.cursor_prev_right_down = gui.cursor_right_down;

    int taskbar_y = gui_screen_h() - TASKBAR_H;
    int smx, smy, smw, smh;
    start_menu_bounds(&smx, &smy, &smw, &smh);

    if (clicked) {
        force_full_redraw = 1;
        if (start_menu_open) {
            if (!(mx >= smx && mx < smx + smw && my >= smy && my < smy + smh)) {
                start_menu_open = 0;
            }
        }

        int start_x = taskbar_start_left();
        if (my >= taskbar_y && mx >= start_x && mx < start_x + 48) {
            start_menu_open = !start_menu_open;
            gui.cursor_prev_down = 1;
            gui_request_full_redraw();
            return;
        }

        if (start_menu_open) {
            for (int i = 0; i < start_item_count; i++) {
                int col = i % 5;
                int row = i / 5;
                int tx = smx + 34 + col * 110;
                int ty = smy + 134 + row * 120;
                if (mx >= tx && mx < tx + 92 && my >= ty && my < ty + 94) {
                    if (start_items[i].launch_mode == 1 && start_items[i].target[0]) gui_launch_link_target(start_items[i].target);
                    else if (start_items[i].fn) start_items[i].fn();
                    start_menu_open = 0;
                    gui_request_full_redraw();
                    return;
                }
            }
            if (mx >= smx + smw - 116 && mx < smx + smw - 24 && my >= smy + smh - 62 && my < smy + smh - 26) {
                start_menu_open = 0;
                gui_activate_lockscreen();
                gui_notify("Session verrouillée");
                gui_request_full_redraw();
                return;
            }
        }

        if (my >= taskbar_y) {
            int tray_x = taskbar_tray_x();
            if (mx >= tray_x && mx < tray_x + taskbar_tray_w()) {
                app_clock_open();
                gui_request_full_redraw();
                return;
            }
            int bx = taskbar_windows_left();
            int shown = 0;
            for (int i = 0; i < gui.window_count; i++) {
                if (!gui.windows[i].visible) continue;
                if (shown >= 6) break;
                if (mx >= bx && mx < bx + 46) {
                    if (gui.windows[i].state == WIN_STATE_MINIMIZED) {
                        gui_restore_window(&gui.windows[i]);
                        gui_focus_window(&gui.windows[i]);
                    } else if (i == gui.focused_window) {
                        gui_minimize_window(&gui.windows[i]);
                    } else {
                        gui_focus_window(&gui.windows[i]);
                    }
                    gui_request_full_redraw();
                    return;
                }
                bx += 54;
                shown++;
            }
            gui_request_full_redraw();
            return;
        }
    }

    if (my >= taskbar_y) {
        int start_x = taskbar_start_left();
        int hovered = (mx >= start_x && mx < start_x + 48);
        if (gui.taskbar_start_hovered != hovered) force_full_redraw = 1;
        gui.taskbar_start_hovered = hovered;
        gui_request_full_redraw();
        return;
    }

    if (gui.taskbar_start_hovered) force_full_redraw = 1;
    gui.taskbar_start_hovered = 0;

    for (int i = 0; i < gui.icon_count; i++) {
        desktop_icon_t *ico = &gui.icons[i];
        int hovered = (mx >= ico->x && mx < ico->x + ICON_SIZE &&
                       my >= ico->y && my < ico->y + ICON_SIZE + 20);
        if (ico->hovered != hovered) force_full_redraw = 1;
        ico->hovered = hovered;
        if (clicked && ico->hovered) {
            int now = timer_ms();
            int same_spot = ((mx > last_click_x ? mx - last_click_x : last_click_x - mx) <= 12) &&
                            ((my > last_click_y ? my - last_click_y : last_click_y - my) <= 12);
            force_full_redraw = 1;
            if (ico->selected || ((now - last_click_time < 400) && same_spot && gui_string_len(ico->name) > 0)) {
                gui_open_icon(ico);
                last_click_time = 0;
            } else {
                for (int j = 0; j < gui.icon_count; j++) gui.icons[j].selected = 0;
                ico->selected = 1;
                last_click_time = now;
                last_click_x = mx; last_click_y = my;
            }
            gui_request_full_redraw();
            return;
        }
    }

    for (int i = gui.window_count-1; i >= 0; i--) {
        window_t *win = &gui.windows[i];
        if (!win->visible || win->state == WIN_STATE_MINIMIZED) continue;

        int in_win = (mx >= win->x && mx < win->x+win->w &&
                      my >= win->y && my < win->y+win->h);
        int in_titlebar = in_win && (my >= win->y && my < win->y + TITLE_BAR_H);

        if (in_titlebar) {
            int btn_w = 28;
            int close_x = win->x + win->w - 12 - btn_w;
            int max_x   = close_x - 6 - btn_w;
            int min_x   = max_x   - 6 - btn_w;
            int btn_y   = win->y + 4;
            int btn_h   = TITLE_BAR_H - 8;

            int close_hover = (mx >= close_x && mx < close_x + btn_w &&
                               my >= btn_y && my < btn_y + btn_h);
            int max_hover = (win->flags & WIN_MAXIMIZABLE) &&
                            (mx >= max_x && mx < max_x + btn_w && my >= btn_y && my < btn_y + btn_h);
            int min_hover = (win->flags & WIN_MINIMIZABLE) &&
                            (mx >= min_x && mx < min_x + btn_w && my >= btn_y && my < btn_y + btn_h);
            if (win->close_btn_hover != close_hover || win->max_btn_hover != max_hover || win->min_btn_hover != min_hover)
                force_full_redraw = 1;
            win->close_btn_hover = close_hover;
            win->max_btn_hover   = max_hover;
            win->min_btn_hover   = min_hover;

            if (clicked) {
                gui_focus_window(win);
                if (win->close_btn_hover) { gui_close_window(win); gui_request_full_redraw(); return; }
                if (win->max_btn_hover) { gui_maximize_window(win); gui_request_full_redraw(); return; }
                if (win->min_btn_hover) { gui_minimize_window(win); gui_request_full_redraw(); return; }
                if (win->flags & WIN_MOVABLE && win->state != WIN_STATE_MAXIMIZED) {
                    win->dragging = 1;
                    win->drag_ox = mx - win->x;
                    win->drag_oy = my - win->y;
                }
                gui_request_full_redraw();
                return;
            }
        } else {
            if (win->close_btn_hover || win->max_btn_hover || win->min_btn_hover) force_full_redraw = 1;
            win->close_btn_hover = win->max_btn_hover = win->min_btn_hover = 0;
        }

        if (in_win && clicked) {
            gui_focus_window(win);
            force_full_redraw = 1;
        }

        if (win->dragging) {
            force_full_redraw = 1;
            if (!gui.cursor_down) {
                win->dragging = 0;
            } else {
                int old_x = win->x, old_y = win->y;
                win->x = mx - win->drag_ox;
                win->y = my - win->drag_oy;
                if (win->x < 0) win->x = 0;
                if (win->y < 0) win->y = 0;
                if (win->x + win->w > gui_screen_w())
                    win->x = gui_screen_w() - win->w;
                if (win->y + win->h > gui_screen_h() - TASKBAR_H)
                    win->y = gui_screen_h() - TASKBAR_H - win->h;
                int dx = win->x - old_x, dy = win->y - old_y;
                for (int j = 0; j < win->widget_count; j++) {
                    win->widgets[j].x += dx;
                    win->widgets[j].y += dy;
                }
            }
        }

        if (in_win || win->dragging || (state->scroll && in_win)) {
            gui_event_t evt;
            if (clicked || released || right_clicked || win->dragging || state->scroll) force_full_redraw = 1;
            evt.dx = state->dx;
            evt.dy = state->dy;
            evt.target = win;
            evt.scroll = state->scroll;

            if (state->scroll)      { evt.type = EVT_SCROLL; evt.button = 0; }
            else if (clicked)       { evt.type = EVT_MOUSEDOWN; evt.button = 1; }
            else if (released)      { evt.type = EVT_MOUSEUP; evt.button = 1; }
            else if (right_clicked) { evt.type = EVT_RCLICK; evt.button = 2; }
            else                    { evt.type = EVT_MOUSEMOVE; evt.button = 0; }

            evt.x = mx; evt.y = my;
            gui_dispatch_event(win, &evt);
            if (win->on_event) win->on_event(NULL, &evt);

            if (in_win) break;
        }
    }

    if (force_full_redraw || clicked || released || right_clicked || start_menu_open) {
        gui_request_full_redraw();
    } else if (mx != old_cursor_x || my != old_cursor_y ||
               gui.cursor_down != old_left || gui.cursor_right_down != old_right) {
        gui_request_cursor_redraw();
    }
}

void gui_handle_key(key_event_t *evt) {

    if (g_lockscreen_active) {
        lockscreen_handle_key(evt);
        return;
    }

    if (evt->alt && evt->scancode == KEY_F12) {
        gui_activate_lockscreen();
        return;
    }

    if (evt->scancode == KEY_SUPER || evt->scancode == KEY_F10 || (evt->ctrl && evt->scancode == KEY_ESC)) {
        start_menu_open = !start_menu_open;
        return;
    }

    if (evt->scancode == KEY_F1) { app_terminal_open(); return; }
    if (evt->scancode == KEY_F2) { app_editor_open();   return; }
    if (evt->scancode == KEY_F3) { app_browser_open();  return; }
    if (evt->scancode == KEY_F4) { app_calculator_open(); return; }
    if (evt->scancode == KEY_F5) { app_filemanager_open(); return; }
    if (evt->scancode == KEY_F6) { app_settings_open(); return; }
    if (evt->scancode == KEY_F7) { app_system_monitor_open(); return; }
    if (evt->scancode == KEY_F8) { app_tutorial_open(); return; }
    if (evt->scancode == KEY_F9) { app_nova_hub_open(); return; }
    if (evt->scancode == KEY_F11) { app_quick_notes_open(); return; }
    if (evt->scancode == KEY_F12 && !evt->alt) { app_symera_open(); return; }

    if (gui.focused_window < 0) return;
    window_t *win = &gui.windows[gui.focused_window];

    if (evt->ctrl && evt->alt) {
        if (evt->scancode == KEY_LEFT) {
            win->save_x = win->x; win->save_y = win->y; win->save_w = win->w; win->save_h = win->h;
            gui_apply_window_rect(win, 0, 0, gui_screen_w() / 2, gui_screen_h() - TASKBAR_H);
            win->state = WIN_STATE_NORMAL;
            gui_notify("Fenêtre ancrée à gauche");
            return;
        }
        if (evt->scancode == KEY_RIGHT) {
            int half = gui_screen_w() / 2;
            win->save_x = win->x; win->save_y = win->y; win->save_w = win->w; win->save_h = win->h;
            gui_apply_window_rect(win, half, 0, gui_screen_w() - half, gui_screen_h() - TASKBAR_H);
            win->state = WIN_STATE_NORMAL;
            gui_notify("Fenêtre ancrée à droite");
            return;
        }
        if (evt->scancode == KEY_UP) {
            gui_maximize_window(win);
            gui_notify("Fenêtre maximisée");
            return;
        }
        if (evt->scancode == KEY_DOWN) {
            gui_restore_window(win);
            gui_notify("Fenêtre restaurée");
            return;
        }
    }

    if (evt->alt && evt->scancode == KEY_F4) {
        gui_close_window(win); return;
    }

    gui_event_t gevt;
    gevt.type = EVT_KEYDOWN;
    gevt.key  = *evt;
    gevt.x = 0; gevt.y = 0;
    gevt.target = win;

    int has_focused_widget = 0;
    for (int i = 0; i < win->widget_count; i++) {
        if (win->widgets[i].visible && win->widgets[i].enabled && win->widgets[i].focused) {
            has_focused_widget = 1;
            break;
        }
    }

    gui_dispatch_event(win, &gevt);
    if (!has_focused_widget && win->on_event) win->on_event(NULL, &gevt);
}

void gui_activate_lockscreen(void) {
    g_lockscreen_active = 1;
    users_lock_screen();
    g_lock.error = 0;
    g_lock.password_len = 0;
    k_memset(g_lock.password_input, 0, 64);
    if (user_sys.current_uid >= 0 && user_sys.current_uid < user_sys.user_count) {
        g_lock.selected_user = user_sys.current_uid;
        k_memcpy(g_lock.username_input, user_sys.users[g_lock.selected_user].username, 64);
        g_lock.username_len = (int)k_strlen(user_sys.users[g_lock.selected_user].username);
    }
}

void gui_notify(const char *msg) {
    gui_string_copy(gui.notif_text, msg, 128);
    gui.notif_time = timer_ms();
    gui_request_full_redraw();
}

void gui_redraw_all(void) {

    if (g_lockscreen_active) {
        lockscreen_draw();
        gui_draw_cursor();
        return;
    }

    gui_draw_wallpaper_cached();

    for (int i = 0; i < gui.icon_count; i++) {
        gui_draw_desktop_icon(&gui.icons[i]);
    }

    int order[MAX_WINDOWS];
    for (int i = 0; i < gui.window_count; i++) order[i] = i;
    for (int i = 0; i < gui.window_count; i++) {
        for (int j = i+1; j < gui.window_count; j++) {
            if (gui.windows[order[i]].zorder > gui.windows[order[j]].zorder) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }
    for (int i = 0; i < gui.window_count; i++) {
        gui_draw_window(&gui.windows[order[i]]);
    }

    gui_draw_taskbar();

    gui_draw_start_menu();

    gui_draw_cursor();
}

void gui_main_loop(void) {
    uint32_t last_draw = 0;
    uint32_t last_cursor_draw = 0;
    uint32_t last_sec = 0;
    uint32_t full_frame_time = 16;
    uint32_t cursor_frame_time = 4;
    int notif_visible = 0;

    while (1) {
        int did_work = 0;

        while (mouse_poll()) {
            did_work = 1;
        }
        uint32_t now = timer_ms();

        if (now - last_sec >= 1000) {
            gui.sec++;
            if (gui.sec >= 60) { gui.sec = 0; gui.min++; }
            if (gui.min >= 60) { gui.min = 0; gui.hour++; }
            if (gui.hour >= 24) { gui.hour = 0; }
            last_sec = now;
            gui_request_full_redraw();
            did_work = 1;
        }

        if (gui.notif_text[0] && now - gui.notif_time < 2600) {
            notif_visible = 1;
        } else if (notif_visible) {
            notif_visible = 0;
            gui_request_full_redraw();
        }

        if (g_full_redraw_needed && now - last_draw >= full_frame_time) {
            gui_redraw_all();
            vbe_swap();
            last_draw = now;
            g_full_redraw_needed = 0;
            g_cursor_redraw_needed = 0;
            did_work = 1;
        } else if (g_cursor_redraw_needed && now - last_cursor_draw >= cursor_frame_time) {
            int old_x = cursor_saved_x;
            int old_y = cursor_saved_y;
            gui_draw_cursor();
            gui_swap_cursor_regions(old_x, old_y, cursor_saved_x, cursor_saved_y);
            last_cursor_draw = now;
            g_cursor_redraw_needed = 0;
            did_work = 1;
        }

        if (!did_work) {

            __asm__ volatile("hlt");
        }
    }
}
