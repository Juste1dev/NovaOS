

#include "../gui/gui.h"
#include "../gui/font.h"
#include "../drivers/vbe.h"
#include "../drivers/keyboard.h"
#include "../drivers/sound.h"
#include "../kernel/timer.h"
#include "../kernel/userspace.h"
#include "../fs/vfs.h"
#include "../net/net.h"
#include "../kernel/users.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

extern uint64_t heap_used(void);
extern uint64_t heap_total(void);

static void misc_itoa(int n, char *buf) {
    if (n < 0) { *buf++ = '-'; n = -n; }
    char tmp[12]; int len=0;
    if (n==0){tmp[len++]='0';}
    while(n>0){tmp[len++]='0'+(n%10);n/=10;}
    for(int i=len-1;i>=0;i--)*buf++=tmp[i];
    *buf=0;
}

static window_t *clock_win = NULL;
static window_t *tutorial_win = NULL;
static window_t *hub_win = NULL;
static window_t *notes_win = NULL;
static window_t *system_win = NULL;
static window_t *userspace_win = NULL;

static void clock_paint(window_t *win) {
    if (!win->visible) return;
    int wx = win->x + 4;
    int wy = win->y + TITLE_BAR_H + 4;
    int ww = win->w - 8;
    int wh = win->h - TITLE_BAR_H - 8;

    vbe_gradient_v(wx, wy, ww, wh, RGB(30,36,54), RGB(18,22,38));
    vbe_rounded_rect_outline(wx, wy, ww, wh, 8, 1, RGB(60,80,120));

    int cx = wx + ww / 2;
    int cy = wy + wh / 2;
    int r  = (ww < wh ? ww : wh) / 2 - 20;

    vbe_circle_fill(cx, cy, r, RGB(24,30,48));
    vbe_circle(cx, cy, r, RGB(80,120,200));
    vbe_circle(cx, cy, r - 3, RGB(50,70,110));

    for (int i = 0; i < 12; i++) {

        static const int sin12[12] = {0,  50,  87,  100,  87, 50, 0, -50, -87, -100, -87, -50};
        static const int cos12[12] = {100, 87, 50, 0, -50, -87, -100, -87, -50, 0, 50, 87};
        int mx = cx + (r - 8) * sin12[i] / 100;
        int my = cy - (r - 8) * cos12[i] / 100;
        vbe_circle_fill(mx, my, (i % 3 == 0) ? 5 : 3, RGB(100,160,255));
    }

    uint32_t ms = timer_ms();
    uint32_t sec = ms / 1000;
    int h = (int)((sec / 3600) % 12);
    int m = (int)((sec / 60) % 60);
    int s = (int)(sec % 60);

    static const int sin60[60] = {
        0,10,21,31,41,50,59,67,74,81,87,92,95,98,99,100,99,98,95,92,87,81,74,67,59,50,41,31,21,10,
        0,-10,-21,-31,-41,-50,-59,-67,-74,-81,-87,-92,-95,-98,-99,-100,-99,-98,-95,-92,-87,-81,-74,-67,-59,-50,-41,-31,-21,-10
    };
    static const int cos60[60] = {
        100,99,98,95,92,87,81,74,67,59,50,41,31,21,10,0,-10,-21,-31,-41,-50,-59,-67,-74,-81,-87,-92,-95,-98,-99,
        -100,-99,-98,-95,-92,-87,-81,-74,-67,-59,-50,-41,-31,-21,-10,0,10,21,31,41,50,59,67,74,81,87,92,95,98,99
    };

    int sx2 = cx + (r - 10) * sin60[s] / 100;
    int sy2 = cy - (r - 10) * cos60[s] / 100;
    vbe_line_thick(cx, cy, sx2, sy2, 1, RGB(255,80,80));

    int mx2 = cx + (r - 20) * sin60[m] / 100;
    int my2 = cy - (r - 20) * cos60[m] / 100;
    vbe_line_thick(cx, cy, mx2, my2, 3, RGB(180,200,255));

    int h_pos = h * 5 + m / 12;
    int hx = cx + (r - 30) * sin60[h_pos % 60] / 100;
    int hy = cy - (r - 30) * cos60[h_pos % 60] / 100;
    vbe_line_thick(cx, cy, hx, hy, 5, RGB(220,230,255));

    vbe_circle_fill(cx, cy, 6, RGB(100,180,255));
    vbe_circle_fill(cx, cy, 3, RGB(255,255,255));

    char tstr[20]; k_memset(tstr, 0, 20);
    char nb[4];
    if (h < 10) { tstr[0]='0'; tstr[1]='0'+(h%10)+'0'-'0'; tstr[1]='0'+h; tstr[2]=0; }
    else { misc_itoa(h, tstr); }
    int tl = 0; while(tstr[tl]) tl++;
    tstr[tl] = ':'; tl++;
    if (m < 10) { tstr[tl]='0'; tl++; }
    misc_itoa(m, nb);
    for(int i=0; nb[i]; i++) tstr[tl++]=nb[i];
    tstr[tl]=':'; tl++;
    if (s < 10) { tstr[tl]='0'; tl++; }
    misc_itoa(s, nb);
    for(int i=0; nb[i]; i++) tstr[tl++]=nb[i];
    tstr[tl]=0;

    int tw = tl * 10;
    font_draw_string(cx - tw/2, wy + wh - 30, tstr, RGB(200,220,255), COLOR_TRANS, FONT_LARGE);
    font_draw_string(cx - 72, wy + wh - 48, "Lun 6 Avr 2026", RGB(120,140,180), COLOR_TRANS, FONT_SMALL);
}

void app_clock_open(void) {
    if (clock_win) { gui_focus_window(clock_win); return; }
    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 340, wh = 380;
    clock_win = gui_create_window(sw/2+200, sh/2-200, ww, wh, "Horloge", WIN_DEFAULT & ~WIN_RESIZABLE);
    if (!clock_win) return;
    clock_win->bg_color = RGB(24,30,48);
    clock_win->on_paint = clock_paint;
    gui_show_window(clock_win);
    gui_focus_window(clock_win);
}

static int misc_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void misc_read_preview(const char *path, char *buf, int max) {
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    if (vfs_exists(path) && !vfs_is_dir(path)) vfs_get_contents(path, buf, max - 1);
}

static void misc_append_text(const char *path, const char *text) {
    static char buf[VFS_MAX_DATA];
    int cur = 0;
    int add = text ? (int)k_strlen(text) : 0;
    if (!text || add <= 0) return;
    k_memset(buf, 0, sizeof(buf));
    if (vfs_exists(path) && !vfs_is_dir(path)) {
        vfs_get_contents(path, buf, sizeof(buf) - 1);
        cur = (int)k_strlen(buf);
    }
    if (cur + add >= VFS_MAX_DATA - 1) return;
    k_memcpy(buf + cur, text, (size_t)add);
    buf[cur + add] = 0;
    vfs_write_file(path, buf, (uint32_t)(cur + add));
}

static int misc_count_lines(const char *text) {
    int count = 0;
    if (!text || !text[0]) return 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') count++;
    }
    if (text[0] && text[k_strlen(text) - 1] != '\n') count++;
    return count;
}

static void misc_extract_tail_lines(const char *src, char *out, int max, int want_lines) {
    int total = misc_count_lines(src);
    int start_line = total > want_lines ? total - want_lines : 0;
    int cur_line = 0;
    int oi = 0;
    if (!out || max <= 0) return;
    out[0] = 0;
    if (!src) return;
    for (int i = 0; src[i] && oi < max - 1; i++) {
        if (cur_line >= start_line) out[oi++] = src[i];
        if (src[i] == '\n') cur_line++;
    }
    out[oi] = 0;
}

static void hub_paint(window_t *win) {
    if (!win->visible) return;
    int wx = win->x + 4, wy = win->y + TITLE_BAR_H + 4;
    int ww = win->w - 8, wh = win->h - TITLE_BAR_H - 8;
    char notes[512];
    misc_read_preview("/home/user/notes.txt", notes, sizeof(notes));

    vbe_gradient_v(wx, wy, ww, wh, RGB(242,247,255), RGB(228,238,255));
    vbe_rounded_rect_outline(wx, wy, ww, wh, 14, 1, RGB(188,206,236));
    vbe_blend_rounded_rect(wx+20, wy+18, ww-40, 98, 18, RGB(255,255,255), 230);
    vbe_gradient_h(wx+34, wy+34, 120, 10, RGB(102,130,255), RGB(110,214,255));
    font_draw_string_shadow(wx+32, wy+26, "Tableau de bord", RGB(30,44,72), FONT_TITLE);
    font_draw_string(wx+32, wy+64, "Raccourcis, etat de session et acces rapides.", RGB(78,98,136), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(wx+32, wy+86, "Raccourcis : F1 Terminal • F3 Navigateur • F7 Systeme • F9 Tableau de bord • F11 Notes", RGB(96,112,146), COLOR_TRANS, FONT_SMALL);

    int cx = wx + 24;
    int cy = wy + 136;
    int card_w = (ww - 68) / 2;
    vbe_blend_rounded_rect(cx, cy, card_w, 150, 18, RGB(255,255,255), 228);
    vbe_rounded_rect_outline(cx, cy, card_w, 150, 18, 1, RGB(200,214,238));
    font_draw_string(cx+18, cy+18, "Resume de session", RGB(38,54,88), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(cx+18, cy+46, "• Interface Full HD avec fenetres et ecran de connexion", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+18, cy+68, "• Terminal, navigateur, stockage local et journal systeme", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+18, cy+90, "• Tableau de bord et notes pour organiser la session", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+18, cy+112, "• Centre Systeme (F7) pour supervision rapide", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);

    int rx = cx + card_w + 20;
    vbe_blend_rounded_rect(rx, cy, card_w, 150, 18, RGB(255,255,255), 228);
    vbe_rounded_rect_outline(rx, cy, card_w, 150, 18, 1, RGB(200,214,238));
    font_draw_string(rx+18, cy+18, "Notes recentes", RGB(38,54,88), COLOR_TRANS, FONT_NORMAL);
    int line_y = cy + 46;
    int drawn = 0;
    char line[96];
    int li = 0;
    for (int i = 0; notes[i] && drawn < 4; i++) {
        if (notes[i] == '\r') continue;
        if (notes[i] == '\n' || li >= 82) {
            line[li] = 0;
            if (line[0]) {
                font_draw_string(rx+18, line_y, line, RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
                line_y += 22;
                drawn++;
            }
            li = 0;
            if (notes[i] == '\n') continue;
        }
        line[li++] = notes[i];
    }
    if (li > 0 && drawn < 4) { line[li] = 0; font_draw_string(rx+18, line_y, line, RGB(86,102,138), COLOR_TRANS, FONT_SMALL); }

    int by = cy + 178;
    vbe_blend_rounded_rect(cx, by, 188, 46, 14, RGB(92,136,255), 235);
    vbe_rounded_rect_outline(cx, by, 188, 46, 14, 1, RGB(138,170,255));
    font_draw_string(cx+34, by+15, "Ouvrir notes de version", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(cx+204, by, 176, 46, 14, RGB(82,198,170), 235);
    vbe_rounded_rect_outline(cx+204, by, 176, 46, 14, 1, RGB(132,226,198));
    font_draw_string(cx+246, by+15, "Applications", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(cx+396, by, 180, 46, 14, RGB(255,186,96), 238);
    vbe_rounded_rect_outline(cx+396, by, 180, 46, 14, 1, RGB(255,214,146));
    font_draw_string(cx+430, by+15, "Notes", RGB(76,54,24), COLOR_TRANS, FONT_NORMAL);

    int fy = by + 70;
    vbe_blend_rounded_rect(cx, fy, ww - 48, 112, 18, RGB(245,249,255), 240);
    vbe_rounded_rect_outline(cx, fy, ww - 48, 112, 18, 1, RGB(200,214,238));
    font_draw_string(cx+18, fy+18, "Checklist de presentation", RGB(38,54,88), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(cx+18, fy+46, "1. Verifier le tableau de bord, le navigateur et les fichiers.", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+18, fy+66, "2. Noter les points a revoir et les correctifs.", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+18, fy+86, "3. Preparer l'ISO, les sources et le journal des modifications.", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
}

static void hub_on_event(widget_t *w, gui_event_t *evt) {
    (void)w;
    if (!hub_win || evt->type != EVT_MOUSEUP) return;
    int wx = hub_win->x + 4, wy = hub_win->y + TITLE_BAR_H + 4;
    int cx = wx + 24;
    int cy = wy + 136;
    int by = cy + 178;
    if (misc_hit(evt->x, evt->y, cx, by, 188, 46)) {
        app_browser_open_url("nova://release-notes");
        gui_notify("Notes de version ouvertes");
    } else if (misc_hit(evt->x, evt->y, cx+204, by, 176, 46)) {
        app_browser_open_url("nova://store");
        gui_notify("Applications ouvertes");
    } else if (misc_hit(evt->x, evt->y, cx+396, by, 180, 46)) {
        app_quick_notes_open();
        gui_notify("Notes ouvertes");
    }
}

void app_nova_hub_open(void) {
    if (hub_win) { gui_focus_window(hub_win); return; }
    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 780, wh = 520;
    hub_win = gui_create_window((sw-ww)/2, (sh-wh)/2-10, ww, wh, "Tableau de bord", WIN_DEFAULT & ~WIN_RESIZABLE);
    if (!hub_win) return;
    hub_win->bg_color = RGB(18,24,36);
    hub_win->on_paint = hub_paint;
    hub_win->on_event = hub_on_event;
    gui_show_window(hub_win);
    gui_focus_window(hub_win);
}

static void notes_paint(window_t *win) {
    if (!win->visible) return;
    int wx = win->x + 4, wy = win->y + TITLE_BAR_H + 4;
    int ww = win->w - 8, wh = win->h - TITLE_BAR_H - 8;
    char notes[1536];
    misc_read_preview("/home/user/notes.txt", notes, sizeof(notes));

    vbe_gradient_v(wx, wy, ww, wh, RGB(255,250,238), RGB(255,244,212));
    vbe_rounded_rect_outline(wx, wy, ww, wh, 12, 1, RGB(230,194,126));
    vbe_blend_rounded_rect(wx+18, wy+16, ww-36, 70, 16, RGB(255,255,255), 210);
    font_draw_string_shadow(wx+28, wy+24, "Notes", RGB(82,62,28), FONT_TITLE);
    font_draw_string(wx+28, wy+58, "Ajoute une note rapide ou ouvre l'editeur pour ecrire plus long.", RGB(120,100,70), COLOR_TRANS, FONT_SMALL);

    int pad_x = wx + 22;
    int pad_y = wy + 102;
    int pad_w = ww - 44;
    int pad_h = wh - 176;
    vbe_blend_rounded_rect(pad_x, pad_y, pad_w, pad_h, 16, RGB(255,251,232), 242);
    vbe_rounded_rect_outline(pad_x, pad_y, pad_w, pad_h, 16, 1, RGB(228,198,136));

    char line[112];
    int li = 0;
    int row = 0;
    for (int i = 0; notes[i] && row < 14; i++) {
        if (notes[i] == '\r') continue;
        if (notes[i] == '\n' || li >= 96) {
            line[li] = 0;
            font_draw_string(pad_x+18, pad_y+16 + row * 22, line, RGB(94,76,42), COLOR_TRANS, FONT_SMALL);
            li = 0;
            row++;
            if (notes[i] == '\n') continue;
        }
        line[li++] = notes[i];
    }
    if (li > 0 && row < 14) {
        line[li] = 0;
        font_draw_string(pad_x+18, pad_y+16 + row * 22, line, RGB(94,76,42), COLOR_TRANS, FONT_SMALL);
    }

    int by = wy + wh - 58;
    vbe_blend_rounded_rect(wx+22, by, 154, 40, 12, RGB(255,214,120), 236);
    vbe_rounded_rect_outline(wx+22, by, 154, 40, 12, 1, RGB(236,186,92));
    font_draw_string(wx+52, by+12, "+ Tâche", RGB(82,58,20), COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx+192, by, 188, 40, 12, RGB(255,238,176), 240);
    vbe_rounded_rect_outline(wx+192, by, 188, 40, 12, 1, RGB(236,204,112));
    font_draw_string(wx+228, by+12, "+ Idee", RGB(82,58,20), COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx+396, by, 176, 40, 12, RGB(112,148,255), 234);
    vbe_rounded_rect_outline(wx+396, by, 176, 40, 12, 1, RGB(150,182,255));
    font_draw_string(wx+430, by+12, "Ouvrir éditeur", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
}

static void notes_on_event(widget_t *w, gui_event_t *evt) {
    (void)w;
    if (!notes_win || evt->type != EVT_MOUSEUP) return;
    int wx = notes_win->x + 4, wy = notes_win->y + TITLE_BAR_H + 4;
    int wh = notes_win->h - TITLE_BAR_H - 8;
    int by = wy + wh - 58;
    if (misc_hit(evt->x, evt->y, wx+22, by, 154, 40)) {
        misc_append_text("/home/user/notes.txt", "- [ ] Nouvelle tâche sprint\n");
        gui_notify("Tâche ajoutée aux notes");
        notes_win->needs_redraw = 1;
    } else if (misc_hit(evt->x, evt->y, wx+192, by, 188, 40)) {
        misc_append_text("/home/user/notes.txt", "- Idee : verifier l'ISO, les sources et la documentation\n");
        gui_notify("Idee enregistree");
        notes_win->needs_redraw = 1;
    } else if (misc_hit(evt->x, evt->y, wx+396, by, 176, 40)) {
        app_editor_open();
        gui_notify("Éditeur ouvert");
    }
}

void app_quick_notes_open(void) {
    if (notes_win) { gui_focus_window(notes_win); return; }
    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 640, wh = 500;
    notes_win = gui_create_window((sw-ww)/2+60, (sh-wh)/2+12, ww, wh, "Notes", WIN_DEFAULT & ~WIN_RESIZABLE);
    if (!notes_win) return;
    notes_win->bg_color = RGB(34,28,18);
    notes_win->on_paint = notes_paint;
    notes_win->on_event = notes_on_event;
    gui_show_window(notes_win);
    gui_focus_window(notes_win);
}

static void system_monitor_paint(window_t *win) {
    if (!win->visible) return;
    int wx = win->x + 4, wy = win->y + TITLE_BAR_H + 4;
    int ww = win->w - 8, wh = win->h - TITLE_BAR_H - 8;
    char ip[20], dns[20], gw[20], boot_count[32], persistence[192], syslog[768], tail[420];
    char mem_used[24], mem_total[24], uptime_m[24], active_user[96];
    uint64_t used = heap_used() / 1024;
    uint64_t total = heap_total() / 1024;
    uint32_t up_min = timer_ms() / 60000;
    user_t *cur = users_get_current();

    net_get_ip_str(net_eth0.ip, ip);
    net_get_ip_str(net_eth0.dns, dns);
    net_get_ip_str(net_eth0.gateway, gw);
    misc_read_preview("/var/log/boot.count", boot_count, sizeof(boot_count));
    misc_read_preview("/proc/persistence", persistence, sizeof(persistence));
    misc_read_preview("/var/log/system.log", syslog, sizeof(syslog));
    misc_extract_tail_lines(syslog, tail, sizeof(tail), 5);
    misc_itoa((int)used, mem_used);
    misc_itoa((int)total, mem_total);
    misc_itoa((int)up_min, uptime_m);
    k_memset(active_user, 0, sizeof(active_user));
    if (cur && cur->fullname[0]) k_memcpy(active_user, cur->fullname, k_strlen(cur->fullname) + 1);
    else if (cur && cur->username[0]) k_memcpy(active_user, cur->username, k_strlen(cur->username) + 1);
    else k_memcpy(active_user, "Aucune session", 14);

    vbe_gradient_v(wx, wy, ww, wh, RGB(236,246,255), RGB(222,236,252));
    vbe_rounded_rect_outline(wx, wy, ww, wh, 14, 1, RGB(182,206,234));
    vbe_blend_rounded_rect(wx+18, wy+18, ww-36, 88, 18, RGB(255,255,255), 228);
    vbe_gradient_h(wx+34, wy+34, 138, 10, RGB(64,196,150), RGB(106,208,255));
    font_draw_string_shadow(wx+32, wy+26, "Centre Système", RGB(24,48,78), FONT_TITLE);
    font_draw_string(wx+32, wy+62, "Supervision en direct : mémoire, réseau, session active, persistance et QA.", RGB(78,98,136), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(wx+32, wy+84, "Raccourcis : F7 pour revenir ici • tableau de bord et notes ci-dessous.", RGB(96,112,146), COLOR_TRANS, FONT_SMALL);

    int cx = wx + 22;
    int cy = wy + 126;
    int card_w = (ww - 64) / 2;
    int card_h = 126;

    vbe_blend_rounded_rect(cx, cy, card_w, card_h, 18, RGB(255,255,255), 224);
    vbe_rounded_rect_outline(cx, cy, card_w, card_h, 18, 1, RGB(190,214,236));
    font_draw_string(cx+18, cy+18, "Mémoire & uptime", RGB(34,56,92), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(cx+18, cy+48, "Heap utilisé :", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+138, cy+48, mem_used, RGB(24,104,88), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(cx+188, cy+48, "Ko", RGB(24,104,88), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+18, cy+72, "Heap total :", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+138, cy+72, mem_total, RGB(24,104,88), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(cx+188, cy+72, "Ko", RGB(24,104,88), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+18, cy+96, "Uptime :", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+138, cy+96, uptime_m, RGB(24,104,88), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(cx+176, cy+96, "min", RGB(24,104,88), COLOR_TRANS, FONT_SMALL);

    int rx = cx + card_w + 20;
    vbe_blend_rounded_rect(rx, cy, card_w, card_h, 18, RGB(255,255,255), 224);
    vbe_rounded_rect_outline(rx, cy, card_w, card_h, 18, 1, RGB(190,214,236));
    font_draw_string(rx+18, cy+18, "Réseau & session", RGB(34,56,92), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(rx+18, cy+48, net_eth0.connected ? "Interface : connectée" : "Interface : hors ligne", net_eth0.connected ? RGB(40,146,96) : RGB(196,92,92), COLOR_TRANS, FONT_SMALL);
    font_draw_string(rx+18, cy+72, ip, RGB(78,98,136), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(rx+18, cy+94, dns, RGB(78,98,136), COLOR_TRANS, FONT_SMALL);
    font_draw_string(rx+138, cy+94, gw, RGB(78,98,136), COLOR_TRANS, FONT_SMALL);
    font_draw_string(rx+18, cy+112, active_user, RGB(30,44,72), COLOR_TRANS, FONT_SMALL);

    int y2 = cy + card_h + 18;
    vbe_blend_rounded_rect(cx, y2, card_w, 124, 18, RGB(255,255,255), 224);
    vbe_rounded_rect_outline(cx, y2, card_w, 124, 18, 1, RGB(190,214,236));
    font_draw_string(cx+18, y2+18, "Persistance & QA", RGB(34,56,92), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(cx+18, y2+48, "Compteur de boot :", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+166, y2+48, boot_count[0] ? boot_count : "0", RGB(30,44,72), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(cx+18, y2+74, persistence[0] ? persistence : "backend=unknown", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    font_draw_string(cx+18, y2+96, "Verifier clavier, tablette et ecran systeme.", RGB(64,132,206), COLOR_TRANS, FONT_SMALL);

    vbe_blend_rounded_rect(rx, y2, card_w, 124, 18, RGB(255,255,255), 224);
    vbe_rounded_rect_outline(rx, y2, card_w, 124, 18, 1, RGB(190,214,236));
    font_draw_string(rx+18, y2+18, "Journal système", RGB(34,56,92), COLOR_TRANS, FONT_NORMAL);
    char line[120];
    int li = 0, row = 0;
    for (int i = 0; tail[i] && row < 4; i++) {
        if (tail[i] == '\r') continue;
        if (tail[i] == '\n' || li >= 56) {
            line[li] = 0;
            if (line[0]) {
                font_draw_string(rx+18, y2+46 + row * 18, line, RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
                row++;
            }
            li = 0;
            if (tail[i] == '\n') continue;
        }
        line[li++] = tail[i];
    }
    if (li > 0 && row < 4) {
        line[li] = 0;
        font_draw_string(rx+18, y2+46 + row * 18, line, RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
    }

    int by = wy + wh - 58;
    vbe_blend_rounded_rect(wx+22, by, 164, 40, 12, RGB(112,148,255), 236);
    vbe_rounded_rect_outline(wx+22, by, 164, 40, 12, 1, RGB(150,182,255));
    font_draw_string(wx+58, by+12, "Tableau de bord", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx+206, by, 176, 40, 12, RGB(255,214,120), 236);
    vbe_rounded_rect_outline(wx+206, by, 176, 40, 12, 1, RGB(236,186,92));
    font_draw_string(wx+238, by+12, "Notes", RGB(82,58,20), COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx+402, by, 208, 40, 12, RGB(76,198,164), 236);
    vbe_rounded_rect_outline(wx+402, by, 208, 40, 12, 1, RGB(122,226,198));
    font_draw_string(wx+448, by+12, "Notes de version", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
}

static void system_monitor_on_event(widget_t *w, gui_event_t *evt) {
    (void)w;
    if (!system_win || (evt->type != EVT_MOUSEUP && evt->type != EVT_CLICK)) return;
    int wx = system_win->x + 4, wy = system_win->y + TITLE_BAR_H + 4;
    int wh = system_win->h - TITLE_BAR_H - 8;
    int by = wy + wh - 58;
    if (misc_hit(evt->x, evt->y, wx+22, by, 164, 40)) {
        app_nova_hub_open();
        gui_notify("Tableau de bord ouvert");
    } else if (misc_hit(evt->x, evt->y, wx+206, by, 176, 40)) {
        app_quick_notes_open();
        gui_notify("Notes ouvertes");
    } else if (misc_hit(evt->x, evt->y, wx+402, by, 208, 40)) {
        app_browser_open_url("nova://release-notes");
        gui_notify("Notes de version ouvertes");
    }
}

void app_system_monitor_open(void) {
    if (system_win) { gui_focus_window(system_win); return; }
    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 760, wh = 520;
    system_win = gui_create_window((sw-ww)/2-10, (sh-wh)/2-4, ww, wh, "System Monitor", WIN_DEFAULT & ~WIN_RESIZABLE);
    if (!system_win) return;
    system_win->bg_color = RGB(16,22,34);
    system_win->on_paint = system_monitor_paint;
    widget_t *mw = gui_add_label(system_win, 0, 0, ww, wh, "");
    if (mw) {
        mw->on_click = system_monitor_on_event;
        mw->focused = 1;
    }
    gui_show_window(system_win);
    gui_focus_window(system_win);
}

static void misc_draw_text_block(int x, int y, int max_lines, const char *text, color_t color) {
    char line[128];
    int li = 0;
    int row = 0;
    int limit = 62;
    for (int i = 0; text && text[i] && row < max_lines; i++) {
        if (text[i] == '\r') continue;
        if (text[i] == '\n' || li >= limit) {
            line[li] = 0;
            font_draw_string(x, y + row * 20, line, color, COLOR_TRANS, FONT_SMALL);
            li = 0;
            row++;
            if (text[i] == '\n') continue;
        }
        line[li++] = text[i];
    }
    if (li > 0 && row < max_lines) {
        line[li] = 0;
        font_draw_string(x, y + row * 20, line, color, COLOR_TRANS, FONT_SMALL);
    }
}

static void userspace_paint(window_t *win) {
    char report[1024];
    char table[1536];
    char count_str[16];
    if (!win->visible) return;
    int wx = win->x + 4;
    int wy = win->y + TITLE_BAR_H + 4;
    int ww = win->w - 8;
    int wh = win->h - TITLE_BAR_H - 8;

    userspace_report(report, sizeof(report));
    userspace_process_table(table, sizeof(table));
    misc_itoa(userspace_process_count(), count_str);

    vbe_gradient_v(wx, wy, ww, wh, RGB(244,248,255), RGB(228,238,255));
    vbe_rounded_rect_outline(wx, wy, ww, wh, 16, 1, RGB(188,206,236));
    vbe_gradient_h(wx+22, wy+22, 154, 12, RGB(96,132,255), RGB(108,224,255));
    font_draw_string_shadow(wx+22, wy+20, "Centre userspace", RGB(30,44,72), FONT_TITLE);
    font_draw_string(wx+22, wy+56, "Services publies, processus actifs et points d'entree de la session.", RGB(78,98,136), COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx+20, wy+92, 238, 102, 18, RGB(255,255,255), 236);
    vbe_rounded_rect_outline(wx+20, wy+92, 238, 102, 18, 1, RGB(200,214,238));
    font_draw_string(wx+40, wy+114, "État", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(wx+40, wy+144, "Userspace actif", RGB(60,180,120), COLOR_TRANS, FONT_LARGE);
    font_draw_string(wx+40, wy+170, count_str, RGB(86,132,255), COLOR_TRANS, FONT_LARGE);
    font_draw_string(wx+72, wy+172, "processus", RGB(96,112,146), COLOR_TRANS, FONT_SMALL);

    vbe_blend_rounded_rect(wx+278, wy+92, ww-298, 102, 18, RGB(255,255,255), 236);
    vbe_rounded_rect_outline(wx+278, wy+92, ww-298, 102, 18, 1, RGB(200,214,238));
    font_draw_string(wx+298, wy+114, "Racines exposées", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(wx+298, wy+144, "/usr/bin  /usr/share/userspace", RGB(78,98,136), COLOR_TRANS, FONT_SMALL);
    font_draw_string(wx+298, wy+166, "/var/run/userspace  /home/user/Documents", RGB(78,98,136), COLOR_TRANS, FONT_SMALL);

    vbe_blend_rounded_rect(wx+20, wy+214, 332, wh-296, 18, RGB(255,255,255), 236);
    vbe_rounded_rect_outline(wx+20, wy+214, 332, wh-296, 18, 1, RGB(200,214,238));
    font_draw_string(wx+38, wy+236, "Résumé", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
    misc_draw_text_block(wx+38, wy+268, 12, report, RGB(86,102,138));

    vbe_blend_rounded_rect(wx+370, wy+214, ww-390, wh-296, 18, RGB(255,255,255), 236);
    vbe_rounded_rect_outline(wx+370, wy+214, ww-390, wh-296, 18, 1, RGB(200,214,238));
    font_draw_string(wx+388, wy+236, "Table des processus", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
    misc_draw_text_block(wx+388, wy+268, 14, table, RGB(78,94,126));

    int by = wy + wh - 62;
    vbe_blend_rounded_rect(wx+20, by, 188, 42, 14, RGB(92,136,255), 235);
    vbe_rounded_rect_outline(wx+20, by, 188, 42, 14, 1, RGB(138,170,255));
    font_draw_string(wx+52, by+13, "Ouvrir le rapport", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);

    vbe_blend_rounded_rect(wx+226, by, 188, 42, 14, RGB(82,198,170), 235);
    vbe_rounded_rect_outline(wx+226, by, 188, 42, 14, 1, RGB(132,226,198));
    font_draw_string(wx+278, by+13, "Terminal", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);

    font_draw_string(wx+438, by+14, "Astuce : commande terminal 'userspace' ou 'ps'.", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
}

static void userspace_on_event(widget_t *w, gui_event_t *evt) {
    (void)w;
    if (!userspace_win || (evt->type != EVT_MOUSEUP && evt->type != EVT_CLICK)) return;
    int wx = userspace_win->x + 4;
    int wy = userspace_win->y + TITLE_BAR_H + 4;
    int wh = userspace_win->h - TITLE_BAR_H - 8;
    int by = wy + wh - 62;
    if (misc_hit(evt->x, evt->y, wx+20, by, 188, 42)) {
        app_browser_open_url("file:///home/user/Documents/Userspace.txt");
        gui_notify("Rapport userspace ouvert");
    } else if (misc_hit(evt->x, evt->y, wx+226, by, 188, 42)) {
        app_terminal_open();
        gui_notify("Terminal ouvert");
    }
}

void app_userspace_open(void) {
    if (userspace_win) { gui_focus_window(userspace_win); return; }
    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 860, wh = 560;
    userspace_win = gui_create_window((sw-ww)/2+40, (sh-wh)/2-6, ww, wh, "Centre userspace", WIN_DEFAULT & ~WIN_RESIZABLE);
    if (!userspace_win) return;
    userspace_win->bg_color = RGB(16,22,34);
    userspace_win->on_paint = userspace_paint;
    widget_t *mw = gui_add_label(userspace_win, 0, 0, ww, wh, "");
    if (mw) {
        mw->on_click = userspace_on_event;
        mw->focused = 1;
    }
    gui_show_window(userspace_win);
    gui_focus_window(userspace_win);
}

static window_t *settings_win = NULL;
static int settings_tab = 0;
static const char *nova_activation_demo_key(const char *edition) {
    if (edition && edition[0] == 'P') return "NOVA6-PRO-2026-1F39";
    return "NOVA6-HOME-2026-22F0";
}

static int nova_activation_eq(const char *a, const char *b) {
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == 0 && b[i] == 0;
}

static int nova_activation_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

static int nova_activation_checksum(const char *body) {
    int sum = 0;
    for (int i = 0; body && body[i]; i++) sum = (sum + ((body[i] * (i + 3)) & 0xFFFF)) & 0xFFFF;
    return sum & 0xFFFF;
}

static void nova_activation_append(char *dst, const char *src, int max) {
    int len = (int)k_strlen(dst);
    int i = 0;
    while (src && src[i] && len + i < max - 1) {
        dst[len + i] = src[i];
        i++;
    }
    dst[len + i] = 0;
}

static int nova_activation_parse(const char *key, char *edition, int edition_max) {
    char body[48];
    int body_len = 0;
    int split = -1;
    int expected = 0;
    if (!key) return 0;
    for (int i = 0; key[i] && body_len < (int)sizeof(body) - 1; i++) {
        char c = key[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        body[body_len++] = c;
    }
    body[body_len] = 0;
    for (int i = body_len - 1; i >= 0; i--) { if (body[i] == '-') { split = i; break; } }
    if (split <= 0 || body_len - split - 1 != 4) return 0;
    if (!(body[0]=='N'&&body[1]=='O'&&body[2]=='V'&&body[3]=='A'&&body[4]=='6'&&body[5]=='-')) return 0;
    if (!(body[split+1] && body[split+2] && body[split+3] && body[split+4])) return 0;
    expected = (nova_activation_hex(body[split+1]) << 12) | (nova_activation_hex(body[split+2]) << 8) | (nova_activation_hex(body[split+3]) << 4) | nova_activation_hex(body[split+4]);
    if (expected < 0) return 0;
    body[split] = 0;
    if (nova_activation_checksum(body) != expected) return 0;
    if (edition && edition_max > 0) {
        if (nova_activation_eq(body, "NOVA6-HOME-2026")) k_strncpy(edition, "Standard", (size_t)edition_max);
        else if (nova_activation_eq(body, "NOVA6-PRO-2026")) k_strncpy(edition, "Atelier", (size_t)edition_max);
        else k_strncpy(edition, "Edition personnalisée", (size_t)edition_max);
    }
    return 1;
}

static int nova_activation_read(char *key_out, int key_max, char *edition_out, int edition_max) {
    char keybuf[64];
    char state[64];
    if (key_out && key_max > 0) key_out[0] = 0;
    if (edition_out && edition_max > 0) edition_out[0] = 0;
    k_memset(keybuf, 0, sizeof(keybuf));
    k_memset(state, 0, sizeof(state));
    if (!vfs_exists("/etc/activation.key")) return 0;
    vfs_get_contents("/etc/activation.key", keybuf, sizeof(keybuf) - 1);
    if (!nova_activation_parse(keybuf, edition_out, edition_max)) return 0;
    if (key_out && key_max > 0) k_strncpy(key_out, keybuf, (size_t)key_max);
    if (vfs_exists("/etc/activation.state")) vfs_get_contents("/etc/activation.state", state, sizeof(state) - 1);
    return state[0] ? 1 : 1;
}

static void nova_activation_write(const char *key) {
    char edition[32];
    char state[160];
    if (!nova_activation_parse(key, edition, sizeof(edition))) return;
    k_memset(state, 0, sizeof(state));
    k_strncpy(state, "status=activated\nedition=", sizeof(state));
    nova_activation_append(state, edition, sizeof(state));
    nova_activation_append(state, "\nchannel=portal\nyear=2026\n", sizeof(state));
    vfs_write_file("/etc/activation.key", key, (uint32_t)k_strlen(key));
    vfs_write_file("/etc/activation.state", state, (uint32_t)k_strlen(state));
}

static void nova_activation_reset(void) {
    const char *key = "UNLICENSED\n";
    const char *state = "status=inactive\nedition=Aucune\nchannel=none\n";
    vfs_write_file("/etc/activation.key", key, (uint32_t)k_strlen(key));
    vfs_write_file("/etc/activation.state", state, (uint32_t)k_strlen(state));
}

static void settings_paint(window_t *win) {
    if (!win->visible) return;
    int wx = win->x + 2;
    int wy = win->y + TITLE_BAR_H;
    int ww = win->w - 4;
    int wh = win->h - TITLE_BAR_H - 4;

    vbe_blend_rect(wx, wy, ww, wh, RGB(246,248,255), 255);

    int sb_w = 184;
    vbe_blend_rect(wx, wy, sb_w, wh, RGB(238,242,255), 255);
    vbe_blend_rect(wx+sb_w, wy, 1, wh, RGB(210,218,240), 200);

    const char *tabs[] = {"Apparence","Verrouillage","Réseau","Comptes","Audio","Système","À propos",NULL};
    for (int i = 0; tabs[i]; i++) {
        int ty = wy + 10 + i * 44;
        if (i == settings_tab) {
            vbe_blend_rounded_rect(wx+4, ty, sb_w-8, 38, 8, RGB(80,140,255), 50);
            vbe_blend_rect(wx+sb_w-4, ty, 4, 38, RGB(80,140,255), 220);
        }
        color_t tc = i == settings_tab ? RGB(40,80,200) : RGB(50,65,100);
        font_draw_string(wx+18, ty+12, tabs[i], tc, COLOR_TRANS, FONT_NORMAL);
    }

    int ca_x = wx + sb_w + 18;
    int ca_y = wy + 12;
    int ca_w = ww - sb_w - 28;

    if (settings_tab == 0) {
        font_draw_string_shadow(ca_x, ca_y, "Apparence", RGB(30,44,72), FONT_TITLE);
        font_draw_string(ca_x, ca_y+42, "Fond du bureau", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);

        const char *names[] = {"Azur", "Dusk", "Émeraude", NULL};
        color_t c1[] = {RGB(236,245,255), RGB(228,234,255), RGB(234,248,244)};
        color_t c2[] = {RGB(180,208,255), RGB(176,164,255), RGB(148,216,190)};
        for (int i = 0; names[i]; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 68;
            int sel = (gui_wallpaper_variant == i);
            vbe_blend_rounded_rect(x, y, 132, 88, 14, c1[i], 240);
            vbe_gradient_v(x, y, 132, 88, c1[i], c2[i]);
            vbe_blend_rounded_rect(x+10, y+16, 62, 16, 8, COLOR_WHITE, 130);
            vbe_blend_rounded_rect(x+12, y+44, 84, 22, 10, COLOR_WHITE, 120);
            vbe_rounded_rect_outline(x, y, 132, 88, 14, sel ? 2 : 1, sel ? RGB(80,140,255) : RGB(200,210,230));
            font_draw_string(x+36, y+100, names[i], sel ? RGB(40,80,200) : RGB(70,85,120), COLOR_TRANS, FONT_SMALL);
        }

        font_draw_string(ca_x, ca_y+188, "Couleur d’accent", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
        color_t accent1[] = {RGB(98,142,255), RGB(132,102,255), RGB(64,196,150)};
        color_t accent2[] = {RGB(116,210,255), RGB(206,124,255), RGB(124,232,188)};
        const char *accent_names[] = {"Bleu Windows", "Violet", "Émeraude", NULL};
        for (int i = 0; accent_names[i]; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 212;
            int sel = (gui_accent_variant == i);
            vbe_gradient_h(x, y, 132, 42, accent1[i], accent2[i]);
            vbe_blend_rounded_rect(x, y, 132, 42, 14, COLOR_WHITE, 20);
            vbe_rounded_rect_outline(x, y, 132, 42, 14, sel ? 2 : 1, sel ? accent1[i] : RGB(200,210,230));
            font_draw_string(x + 16, y + 13, accent_names[i], COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
        }

        font_draw_string(ca_x, ca_y+284, "Barre des tâches et ancrage", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
        vbe_blend_rounded_rect(ca_x, ca_y+308, ca_w-30, 82, 16, RGB(255,255,255), 236);
        vbe_rounded_rect_outline(ca_x, ca_y+308, ca_w-30, 82, 16, 1, RGB(200,212,238));
        font_draw_string(ca_x+18, ca_y+326, "Full HD natif, plateau horloge cliquable et barre centrée", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+18, ca_y+348, "Ctrl+Alt+←/→/↑/↓ ancre, maximise et restaure les fenêtres actives.", RGB(96,112,146), COLOR_TRANS, FONT_SMALL);
        font_draw_string(ca_x+18, ca_y+366, "Choisissez un fond et une couleur d’accent puis revenez au bureau.", RGB(96,112,146), COLOR_TRANS, FONT_SMALL);

        font_draw_string(ca_x, ca_y+406, "Style de barre des tâches", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
        const char *tb_names[] = {"Verre", "Graphite", "Clair", NULL};
        for (int i = 0; tb_names[i]; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 430;
            int sel = (gui_taskbar_variant == i);
            color_t fill = i == 2 ? RGB(240,246,255) : (i == 1 ? RGB(40,42,52) : RGB(24,34,54));
            color_t tc = i == 2 ? RGB(52,68,96) : COLOR_WHITE;
            vbe_blend_rounded_rect(x, y, 132, 38, 14, fill, 236);
            vbe_rounded_rect_outline(x, y, 132, 38, 14, sel ? 2 : 1, sel ? RGB(80,140,255) : RGB(200,210,230));
            font_draw_string(x + 34, y + 12, tb_names[i], tc, COLOR_TRANS, FONT_SMALL);
        }

        font_draw_string(ca_x, ca_y+484, "Souris", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
        const char *mouse_names[] = {"Précise", "Équilibrée", "Rapide", NULL};
        for (int i = 0; mouse_names[i]; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 508;
            int sel = (mouse_get_speed_preset() == i);
            vbe_blend_rounded_rect(x, y, 132, 38, 14, sel ? RGB(80,140,255) : RGB(245,248,255), sel ? 226 : 236);
            vbe_rounded_rect_outline(x, y, 132, 38, 14, sel ? 2 : 1, sel ? RGB(80,140,255) : RGB(200,210,230));
            font_draw_string(x + 24, y + 12, mouse_names[i], sel ? COLOR_WHITE : RGB(70,85,120), COLOR_TRANS, FONT_SMALL);
        }

    } else if (settings_tab == 1) {
        font_draw_string_shadow(ca_x, ca_y, "Écran de verrouillage", RGB(30,44,72), FONT_TITLE);
        font_draw_string(ca_x, ca_y+42, "Style visuel", RGB(50,65,100), COLOR_TRANS, FONT_NORMAL);
        const char *styles[] = {"Aurora", "Midnight", "Sunrise", NULL};
        color_t a1[] = {RGB(16,24,48), RGB(10,12,28), RGB(42,24,54)};
        color_t a2[] = {RGB(60,120,255), RGB(120,80,255), RGB(255,140,110)};
        for (int i = 0; styles[i]; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 68;
            int sel = (gui_lockscreen_variant == i);
            vbe_gradient_v(x, y, 132, 96, a1[i], blend_color(a2[i], a1[i], 60));
            vbe_blend_rounded_rect(x+26, y+20, 80, 52, 14, COLOR_WHITE, 26);
            vbe_circle_fill(x+66, y+42, 14, a2[i]);
            vbe_rounded_rect_outline(x, y, 132, 96, 14, sel ? 2 : 1, sel ? RGB(80,140,255) : RGB(200,210,230));
            font_draw_string(x+30, y+108, styles[i], sel ? RGB(40,80,200) : RGB(70,85,120), COLOR_TRANS, FONT_SMALL);
        }
        vbe_blend_rounded_rect(ca_x, ca_y+190, ca_w-30, 92, 16, RGB(255,255,255), 236);
        vbe_rounded_rect_outline(ca_x, ca_y+190, ca_w-30, 92, 16, 1, RGB(200,212,238));
        font_draw_string(ca_x+18, ca_y+208, "Choisissez un visuel puis revenez à l’écran d’accueil.", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+18, ca_y+232, "La sélection de compte reste disponible en bas de l’écran.", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
        font_draw_string(ca_x+18, ca_y+250, "Alt+F12 verrouille immédiatement la session courante.", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
        font_draw_string(ca_x+18, ca_y+268, "Le changement est appliqué sans redémarrage.", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);

    } else if (settings_tab == 2) {
        font_draw_string_shadow(ca_x, ca_y, "Réseau", RGB(30,44,72), FONT_TITLE);
        char ip_str[20]; net_get_ip_str(net_eth0.ip, ip_str);
        char mac_str[20]; net_get_mac_str(net_eth0.mac, mac_str);
        struct { const char *lbl; const char *val; } ninfo[] = {
            {"Interface:", "eth0 (RTL8139)"},
            {"Adresse IP:", ip_str},
            {"Masque réseau:", "255.255.255.0"},
            {"Passerelle:", "10.0.2.1"},
            {"DNS:", "8.8.8.8"},
            {"Adresse MAC:", mac_str},
            {"Statut:", net_eth0.connected ? "Connecté" : "Déconnecté"},
            {NULL,NULL}
        };
        for (int i = 0; ninfo[i].lbl; i++) {
            int ny = ca_y + 44 + i * 32;
            font_draw_string(ca_x, ny, ninfo[i].lbl, RGB(80,100,140), COLOR_TRANS, FONT_NORMAL);
            color_t vc = RGB(40,50,80);
            if (i == 6) vc = net_eth0.connected ? RGB(60,180,100) : RGB(220,60,60);
            font_draw_string(ca_x+140, ny, ninfo[i].val, vc, COLOR_TRANS, FONT_NORMAL);
        }
        vbe_blend_rounded_rect(ca_x, ca_y+286, ca_w-30, 72, 14, RGB(255,255,255), 236);
        vbe_rounded_rect_outline(ca_x, ca_y+286, ca_w-30, 72, 14, 1, RGB(200,212,238));
        font_draw_string(ca_x+18, ca_y+306, "État du réseau", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+18, ca_y+328, "Les paramètres affichés ici sont ceux de la session actuelle.", RGB(96,112,146), COLOR_TRANS, FONT_SMALL);

    } else if (settings_tab == 3) {
        font_draw_string_shadow(ca_x, ca_y, "Comptes et sessions", RGB(30,44,72), FONT_TITLE);
        int list_y = ca_y + 50;
        for (int i = 0; i < user_sys.user_count; i++) {
            user_t *u = &user_sys.users[i];
            int y = list_y + i * 74;
            vbe_blend_rounded_rect(ca_x, y, ca_w-30, 62, 14, RGB(255,255,255), 236);
            vbe_rounded_rect_outline(ca_x, y, ca_w-30, 62, 14, 1, RGB(200,212,238));
            color_t badge = u->role == USER_ROLE_ADMIN ? RGB(80,140,255) : (u->role == USER_ROLE_GUEST ? RGB(120,140,170) : RGB(60,180,120));
            vbe_circle_fill(ca_x+28, y+30, 18, badge);
            font_draw_string(ca_x+22, y+22, "U", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
            font_draw_string(ca_x+56, y+14, u->fullname[0] ? u->fullname : u->username, RGB(30,44,72), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(ca_x+56, y+34, u->username, RGB(96,112,146), COLOR_TRANS, FONT_SMALL);
            font_draw_string(ca_x+ca_w-180, y+22,
                u->role == USER_ROLE_ADMIN ? "Administrateur" : (u->role == USER_ROLE_GUEST ? "Invité" : "Utilisateur"),
                badge, COLOR_TRANS, FONT_SMALL);
        }
        int by = ca_y + 300;
        vbe_blend_rounded_rect(ca_x, by, 250, 42, 12, RGB(80,140,255), 230);
        vbe_rounded_rect_outline(ca_x, by, 250, 42, 12, 1, RGB(120,168,255));
        font_draw_string(ca_x+28, by+13, "Verrouiller maintenant", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
        vbe_blend_rounded_rect(ca_x+266, by, ca_w-296, 42, 12, RGB(245,248,255), 236);
        vbe_rounded_rect_outline(ca_x+266, by, ca_w-296, 42, 12, 1, RGB(200,212,238));
        font_draw_string(ca_x+284, by+13, "Comptes disponibles sur l’écran d’accueil", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);

    } else if (settings_tab == 4) {
        char summary[128];
        sound_fill_summary(summary, sizeof(summary));
        font_draw_string_shadow(ca_x, ca_y, "Audio", RGB(30,44,72), FONT_TITLE);
        vbe_blend_rounded_rect(ca_x, ca_y+46, ca_w-30, 88, 16, RGB(255,255,255), 236);
        vbe_rounded_rect_outline(ca_x, ca_y+46, ca_w-30, 88, 16, 1, RGB(200,212,238));
        font_draw_string(ca_x+18, ca_y+64, "Backend actif", RGB(80,100,140), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+170, ca_y+64, sound_output_backend(), RGB(40,50,80), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+18, ca_y+90, "Pile de pilotes", RGB(80,100,140), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+170, ca_y+90, sound_driver_stack(), RGB(40,50,80), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+18, ca_y+116, "Résumé", RGB(80,100,140), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+170, ca_y+116, summary, RGB(40,50,80), COLOR_TRANS, FONT_SMALL);

        vbe_blend_rounded_rect(ca_x, ca_y+156, 232, 78, 14, RGB(245,248,255), 236);
        vbe_rounded_rect_outline(ca_x, ca_y+156, 232, 78, 14, 1, RGB(200,212,238));
        font_draw_string(ca_x+18, ca_y+176, "PC speaker", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+18, ca_y+202, sound_pc_speaker_ready() ? "Actif" : "Indisponible", sound_pc_speaker_ready() ? RGB(60,180,100) : RGB(220,60,60), COLOR_TRANS, FONT_NORMAL);

        vbe_blend_rounded_rect(ca_x+248, ca_y+156, 232, 78, 14, RGB(245,248,255), 236);
        vbe_rounded_rect_outline(ca_x+248, ca_y+156, 232, 78, 14, 1, RGB(200,212,238));
        font_draw_string(ca_x+266, ca_y+176, "Sound Blaster 16", RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x+266, ca_y+202, sound_sb16_detected() ? "Detecte" : "Non detecte", sound_sb16_detected() ? RGB(60,180,100) : RGB(220,140,60), COLOR_TRANS, FONT_NORMAL);

        vbe_blend_rounded_rect(ca_x, ca_y+260, 260, 42, 12, RGB(80,140,255), 230);
        vbe_rounded_rect_outline(ca_x, ca_y+260, 260, 42, 12, 1, RGB(120,168,255));
        font_draw_string(ca_x+34, ca_y+273, "Relancer le test audio", COLOR_WHITE, COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x, ca_y+324, "Le test relance la sortie sonore de la session courante.", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
        font_draw_string(ca_x, ca_y+346, "Touches 1 à 7 ou ↑↓ pour changer d’onglet.", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);

    } else if (settings_tab == 5) {
        font_draw_string_shadow(ca_x, ca_y, "Système", RGB(30,44,72), FONT_TITLE);
        extern uint64_t heap_used(void);
        extern uint64_t heap_total(void);
        uint64_t used = heap_used();
        uint64_t total = heap_total();
        char u_str[20]; k_memset(u_str, 0, 20);
        misc_itoa((int)(used/1024), u_str);
        int ul = 0; while(u_str[ul]) ul++; u_str[ul++]=' '; u_str[ul++]='K'; u_str[ul++]='o'; u_str[ul]=0;
        char t_str[20]; k_memset(t_str, 0, 20);
        misc_itoa((int)(total/1024), t_str);
        int tl2=0; while(t_str[tl2]) tl2++; t_str[tl2++]=' '; t_str[tl2++]='K'; t_str[tl2++]='o'; t_str[tl2]=0;
        struct { const char *k; const char *v; } sinfo[] = {
            {"OS:", "NovaOS 6.0"},
            {"Architecture:", "x86_64 (64-bit)"},
            {"Noyau:", "Bare-metal graphique"},
            {"Mémoire utilisée:", u_str},
            {"Mémoire totale:", t_str},
            {"Affichage:", "VBE framebuffer 32 bpp · Full HD natif"},
            {"Audio:", sound_output_backend()},
            {"Système de fichiers:", "FAT32 en RAM"},
            {NULL,NULL}
        };
        for (int i = 0; sinfo[i].k; i++) {
            int sy2 = ca_y + 44 + i * 30;
            if (i % 2 == 0) vbe_blend_rect(ca_x, sy2-2, ca_w-20, 28, RGB(248,250,255), 180);
            font_draw_string(ca_x+8, sy2+6, sinfo[i].k, RGB(80,100,140), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(ca_x+190, sy2+6, sinfo[i].v, RGB(40,50,80), COLOR_TRANS, FONT_NORMAL);
        }
        {
            char act_key[64];
            char act_edition[32];
            int active = nova_activation_read(act_key, sizeof(act_key), act_edition, sizeof(act_edition));
            int ay = ca_y + 302;
            vbe_blend_rounded_rect(ca_x, ay, ca_w-28, 164, 18, RGB(255,255,255), 236);
            vbe_rounded_rect_outline(ca_x, ay, ca_w-28, 164, 18, 1, RGB(200,212,238));
            font_draw_string(ca_x+18, ay+18, "Edition", RGB(34,56,92), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(ca_x+18, ay+46, active ? "État : activé" : "État : non activé", active ? RGB(50,168,104) : RGB(198,96,82), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(ca_x+180, ay+46, active ? act_edition : "Non definie", RGB(54,74,112), COLOR_TRANS, FONT_SMALL);
            font_draw_string(ca_x+18, ay+72, active ? act_key : "Clé : aucune", RGB(86,102,138), COLOR_TRANS, FONT_SMALL);
            font_draw_string(ca_x+18, ay+94, "L'outil local de gestion des editions genere les memes cles que ce panneau.", RGB(96,112,146), COLOR_TRANS, FONT_SMALL);
            vbe_blend_rounded_rect(ca_x+18, ay+116, 164, 34, 12, RGB(88,132,255), 230);
            vbe_rounded_rect_outline(ca_x+18, ay+116, 164, 34, 12, 1, RGB(128,164,255));
            font_draw_string(ca_x+44, ay+127, "Activer Standard", COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
            vbe_blend_rounded_rect(ca_x+196, ay+116, 164, 34, 12, RGB(82,198,170), 230);
            vbe_rounded_rect_outline(ca_x+196, ay+116, 164, 34, 12, 1, RGB(130,226,198));
            font_draw_string(ca_x+228, ay+127, "Activer Atelier", COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
            vbe_blend_rounded_rect(ca_x+374, ay+116, 156, 34, 12, RGB(244,246,252), 236);
            vbe_rounded_rect_outline(ca_x+374, ay+116, 156, 34, 12, 1, RGB(200,212,238));
            font_draw_string(ca_x+418, ay+127, "Réinitialiser", RGB(78,94,124), COLOR_TRANS, FONT_SMALL);
        }

    } else if (settings_tab == 6) {
        font_draw_string_shadow(ca_x, ca_y, "À propos", RGB(30,44,72), FONT_TITLE);
        vbe_blend_rounded_rect(ca_x + ca_w/2 - 140, ca_y+46, 280, 124, 16, RGB(240,246,255), 240);
        vbe_gradient_h(ca_x + ca_w/2 - 54, ca_y+56, 48, 8, RGB(104,132,255), RGB(110,210,255));
        font_draw_string_shadow(ca_x + ca_w/2 - 114, ca_y+58, "NovaOS 6.0", RGB(30,44,72), FONT_LARGE);
        font_draw_string(ca_x + ca_w/2 - 66, ca_y+92, "Version 6.0", RGB(80,100,140), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(ca_x + ca_w/2 - 72, ca_y+112, "Session x86_64", RGB(100,120,160), COLOR_TRANS, FONT_SMALL);
        font_draw_string(ca_x, ca_y+194, "Environnement graphique local pour verification et presentation.", RGB(70,85,120), COLOR_TRANS, FONT_SMALL);
        font_draw_string(ca_x, ca_y+220, "Tableau de bord, notes, navigateur, fichiers, audio et comptes sont accessibles depuis le menu Demarrer.", RGB(70,85,120), COLOR_TRANS, FONT_SMALL);
    }
}

static void settings_on_key(widget_t *w, gui_event_t *evt) {
    (void)w;
    if (evt->type != EVT_KEYDOWN && evt->type != EVT_CHAR) return;

    if (evt->key.ascii >= '1' && evt->key.ascii <= '7') {
        settings_tab = evt->key.ascii - '1';
        if (settings_win) settings_win->needs_redraw = 1;
        return;
    }
    if (evt->key.scancode == KEY_UP || evt->key.scancode == KEY_LEFT) {
        settings_tab = (settings_tab + 6) % 7;
        if (settings_win) settings_win->needs_redraw = 1;
        return;
    }
    if (evt->key.scancode == KEY_DOWN || evt->key.scancode == KEY_RIGHT) {
        settings_tab = (settings_tab + 1) % 7;
        if (settings_win) settings_win->needs_redraw = 1;
        return;
    }
    if ((evt->key.scancode == KEY_ENTER || evt->key.ascii == '\n' || evt->key.ascii == '\r') && settings_tab == 4) {
        sound_run_self_test();
        gui_notify("Test audio relancé");
        if (settings_win) settings_win->needs_redraw = 1;
        return;
    }
}

static void settings_on_click(widget_t *w, gui_event_t *evt) {
    (void)w;
    if (evt->type != EVT_CLICK) return;
    if (!settings_win) return;
    int mx = evt->x - settings_win->x - 2;
    int my = evt->y - settings_win->y - TITLE_BAR_H;
    int sb_w = 184;
    if (mx >= 0 && mx < sb_w) {
        int tab = (my - 10) / 44;
        if (tab >= 0 && tab < 7) {
            settings_tab = tab;
            settings_win->needs_redraw = 1;
        }
        return;
    }

    int ca_x = sb_w + 18;
    int ca_y = 12;

    if (settings_tab == 0) {
        for (int i = 0; i < 3; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 68;
            if (mx >= x && mx < x + 132 && my >= y && my < y + 88) {
                gui_wallpaper_variant = i;
                gui_preferences_save();
                gui_notify("Fond du bureau mis à jour");
                settings_win->needs_redraw = 1;
                return;
            }
        }
        for (int i = 0; i < 3; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 212;
            if (mx >= x && mx < x + 132 && my >= y && my < y + 42) {
                gui_accent_variant = i;
                gui_preferences_save();
                gui_notify("Couleur d’accent appliquée");
                settings_win->needs_redraw = 1;
                return;
            }
        }
        for (int i = 0; i < 3; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 430;
            if (mx >= x && mx < x + 132 && my >= y && my < y + 38) {
                gui_taskbar_variant = i;
                gui_preferences_save();
                gui_notify("Style de barre des tâches appliqué");
                settings_win->needs_redraw = 1;
                return;
            }
        }
        for (int i = 0; i < 3; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 508;
            if (mx >= x && mx < x + 132 && my >= y && my < y + 38) {
                mouse_set_speed_preset(i);
                gui_preferences_save();
                gui_notify("Réglage souris enregistré");
                settings_win->needs_redraw = 1;
                return;
            }
        }
    } else if (settings_tab == 1) {
        for (int i = 0; i < 3; i++) {
            int x = ca_x + i * 154;
            int y = ca_y + 68;
            if (mx >= x && mx < x + 132 && my >= y && my < y + 96) {
                gui_lockscreen_variant = i;
                gui_preferences_save();
                gui_notify("Style de verrouillage appliqué");
                settings_win->needs_redraw = 1;
                return;
            }
        }
    } else if (settings_tab == 3) {
        int by = ca_y + 300;
        if (mx >= ca_x && mx < ca_x + 250 && my >= by && my < by + 42) {
            gui_activate_lockscreen();
            gui_notify("Écran verrouillé");
            settings_win->needs_redraw = 1;
            return;
        }
    } else if (settings_tab == 4) {
        int by = ca_y + 260;
        if (mx >= ca_x && mx < ca_x + 260 && my >= by && my < by + 42) {
            sound_run_self_test();
            gui_notify("Test audio relancé");
            settings_win->needs_redraw = 1;
            return;
        }
    } else if (settings_tab == 5) {
        int ay = ca_y + 302;
        if (mx >= ca_x + 18 && mx < ca_x + 182 && my >= ay + 116 && my < ay + 150) {
            nova_activation_write(nova_activation_demo_key("HOME"));
            gui_notify("Edition Standard appliquee");
            settings_win->needs_redraw = 1;
            return;
        }
        if (mx >= ca_x + 196 && mx < ca_x + 360 && my >= ay + 116 && my < ay + 150) {
            nova_activation_write(nova_activation_demo_key("PRO"));
            gui_notify("Edition Atelier appliquee");
            settings_win->needs_redraw = 1;
            return;
        }
        if (mx >= ca_x + 374 && mx < ca_x + 530 && my >= ay + 116 && my < ay + 150) {
            nova_activation_reset();
            gui_notify("Edition reinitialisee");
            settings_win->needs_redraw = 1;
            return;
        }
    }
}

void app_settings_open(void) {
    if (settings_win) { gui_focus_window(settings_win); return; }
    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 820, wh = 580;
    settings_win = gui_create_window((sw-ww)/2, (sh-wh)/2, ww, wh, "Paramètres", WIN_DEFAULT);
    if (!settings_win) return;
    settings_win->bg_color = RGB(16,22,34);
    settings_win->on_paint = settings_paint;

    widget_t *mw = gui_add_label(settings_win, 0, 0, ww, wh, "");
    if (mw) {
        mw->on_click = settings_on_click;
        mw->on_keydown = settings_on_key;
        mw->focused = 1;
    }

    gui_show_window(settings_win);
    gui_focus_window(settings_win);
}

static void tutorial_paint(window_t *win) {
    if (!win->visible) return;
    int wx = win->x + 4;
    int wy = win->y + TITLE_BAR_H + 4;
    int ww = win->w - 8;
    int wh = win->h - TITLE_BAR_H - 8;
    user_t *cur = users_get_current();
    const char *name = (cur && cur->fullname[0]) ? cur->fullname : (cur && cur->username[0] ? cur->username : "utilisateur");

    vbe_gradient_v(wx, wy, ww, wh, RGB(248,252,255), RGB(232,242,255));
    vbe_rounded_rect_outline(wx, wy, ww, wh, 12, 1, RGB(200,214,238));

    vbe_blend_rounded_rect(wx+18, wy+18, ww-36, 92, 18, RGB(255,255,255), 228);
    vbe_gradient_h(wx+36, wy+36, 84, 10, RGB(104,132,255), RGB(110,210,255));
    font_draw_string_shadow(wx+34, wy+28, "Guide", RGB(30,44,72), FONT_TITLE);
    font_draw_string(wx+34, wy+64, name, RGB(74,96,132), COLOR_TRANS, FONT_NORMAL);
    font_draw_string(wx+34, wy+82, "Actions essentielles.", RGB(96,112,146), COLOR_TRANS, FONT_SMALL);

    const char *steps[] = {
        "1. Déverrouillage : entrez votre mot de passe sur l'écran d'accueil.",
        "2. Bureau : utilisez le menu démarrer ou les icônes à gauche.",
        "3. Raccourcis : F1 Terminal, F5 Fichiers, F6 Paramètres.",
        "4. Web : F3 ouvre le navigateur.",
        "5. Session : F12 verrouille la session.",
        NULL
    };

    for (int i = 0; steps[i]; i++) {
        int y = wy + 134 + i * 54;
        vbe_blend_rounded_rect(wx+24, y, ww-48, 40, 12, RGB(255,255,255), 214);
        vbe_rounded_rect_outline(wx+24, y, ww-48, 40, 12, 1, RGB(204,214,232));
        font_draw_string(wx+38, y+13, steps[i], RGB(42,58,86), COLOR_TRANS, FONT_NORMAL);
    }

    vbe_blend_rounded_rect(wx+24, wy+wh-86, ww-48, 58, 16, RGB(240,246,255), 240);
    vbe_rounded_rect_outline(wx+24, wy+wh-86, ww-48, 58, 16, 1, RGB(200,214,238));
    font_draw_string(wx+42, wy+wh-48, "Alt+F12 verrouille immédiatement la session.", RGB(92,108,146), COLOR_TRANS, FONT_SMALL);
}

void app_tutorial_open(void) {
    if (tutorial_win) { gui_focus_window(tutorial_win); return; }
    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 780, wh = 470;
    tutorial_win = gui_create_window((sw-ww)/2-20, (sh-wh)/2-10, ww, wh, "Guide", WIN_DEFAULT & ~WIN_RESIZABLE);
    if (!tutorial_win) return;
    tutorial_win->bg_color = RGB(16,22,34);
    tutorial_win->on_paint = tutorial_paint;
    gui_show_window(tutorial_win);
    gui_focus_window(tutorial_win);
}

static void about_paint(window_t *win) {
    if (!win->visible) return;
    int wx = win->x + 4;
    int wy = win->y + TITLE_BAR_H + 4;
    int ww = win->w - 8;
    int wh = win->h - TITLE_BAR_H - 8;

    vbe_gradient_v(wx, wy, ww, wh, RGB(248,252,255), RGB(232,242,255));
    vbe_rounded_rect_outline(wx, wy, ww, wh, 10, 1, RGB(200,214,238));

    vbe_blend_rounded_rect(wx+ww/2-120, wy+20, 240, 100, 16, RGB(240,246,255), 240);
    vbe_gradient_h(wx+ww/2-40, wy+28, 80, 10, RGB(104,132,255), RGB(110,210,255));
    font_draw_string_shadow(wx+ww/2-110, wy+28, "Système", RGB(30,44,72), FONT_TITLE);

    struct { const char *lbl; const char *val; } info[] = {
        {"Noyau",      "x86_64"},
        {"Architecture","x86_64 – bare-metal"},
        {"Interface",  "VBE/VESA 1920x1080x32"},
        {"Filesystem", "FAT32 RAM (VFS)"},
        {"Mémoire",    "64 Mo min. recommandé"},
        {"Réseau",     "RTL8139 (PCI)"},
        {"Interface",  "graphique"},
        {NULL,NULL}
    };

    for (int i = 0; info[i].lbl; i++) {
        int iy = wy + 140 + i * 28;
        if (i % 2 == 0) vbe_blend_rect(wx+10, iy-2, ww-20, 26, RGB(240,244,255), 160);
        font_draw_string(wx+20, iy+5, info[i].lbl, RGB(80,100,140), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(wx+180, iy+5, info[i].val, RGB(40,55,90), COLOR_TRANS, FONT_NORMAL);
    }

}

void app_about_open(void) {
    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 560, wh = 460;
    window_t *w = gui_create_window((sw-ww)/2+60, (sh-wh)/2+30, ww, wh, "Système", WIN_DEFAULT & ~WIN_RESIZABLE);
    if (!w) return;
    w->bg_color = RGB(16,22,34);
    w->on_paint = about_paint;
    gui_show_window(w);
    gui_focus_window(w);
}
