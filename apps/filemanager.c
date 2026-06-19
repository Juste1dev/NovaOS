

#include "../gui/gui.h"
#include "../gui/font.h"
#include "../drivers/vbe.h"
#include "../drivers/keyboard.h"
#include "../kernel/timer.h"
#include "../fs/vfs.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

#define FM_MAX_ENTRIES  256
#define FM_NAME_LEN     256
#define FM_PATH_LEN     512

typedef struct {
    char   path[FM_PATH_LEN];
    char   entries[FM_MAX_ENTRIES][FM_NAME_LEN];
    int    is_dir[FM_MAX_ENTRIES];
    int    entry_count;
    int    selected;
    int    scroll;
    window_t *win;
    int    need_redraw;
    int    breadcrumb_hover;
    char   status[256];

    char   history[16][FM_PATH_LEN];
    int    hist_pos;
    int    hist_count;
} filemanager_t;

static filemanager_t g_fm;
static int fm_open_count = 0;

static int fm_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }
static void fm_strcpy(char *d, const char *s, int m) {
    int i=0; while(s[i]&&i<m-1){d[i]=s[i];i++;} d[i]=0;
}

static void fm_load_dir(filemanager_t *fm, const char *path) {
    fm_strcpy(fm->path, path, FM_PATH_LEN);
    fm->entry_count = vfs_list_dir(path, fm->entries, fm->is_dir, FM_MAX_ENTRIES);
    if (fm->entry_count < 0) fm->entry_count = 0;
    fm->selected = 0;
    fm->scroll = 0;
    fm->need_redraw = 1;

    char buf[256]; k_memset(buf, 0, 256);
    char nb[16];
    int n = fm->entry_count;

    int nd=0, nf=0;
    for(int i=0;i<n;i++) { if(fm->is_dir[i]) nd++; else nf++; }
    k_memcpy(buf, "  ", 3);
    char tmp[8];
    tmp[0]='0'+nd/10; tmp[1]='0'+nd%10; tmp[2]=0;
    if(nd>=10) k_memcpy(buf+2, tmp, 3);
    else { buf[2]='0'+nd; buf[3]=0; }
    int bl = fm_strlen(buf);
    k_memcpy(buf+bl, " dossier(s),  ", 15); bl=fm_strlen(buf);
    tmp[0]='0'+nf/10; tmp[1]='0'+nf%10; tmp[2]=0;
    if(nf>=10) k_memcpy(buf+bl, tmp, 3);
    else { buf[bl]='0'+nf; buf[bl+1]=0; }
    bl=fm_strlen(buf);
    k_memcpy(buf+bl, " fichier(s)", 12);
    fm_strcpy(fm->status, buf, 256);
    (void)nb;
}

static void fm_navigate(filemanager_t *fm, const char *path) {

    if (fm->hist_count < 16) {
        fm_strcpy(fm->history[fm->hist_count], path, FM_PATH_LEN);
        fm->hist_count++;
        fm->hist_pos = fm->hist_count - 1;
    }
    fm_load_dir(fm, path);
}

static void fm_draw_icon(int x, int y, int is_dir, color_t accent) {
    if (is_dir) {

        vbe_blend_rounded_rect(x, y+4, 32, 22, 4, accent, 230);
        vbe_blend_rounded_rect(x, y, 14, 6, 3, accent, 200);
        vbe_blend_rect(x+2, y+6, 28, 18, RGB(255,255,255), 25);
    } else {

        vbe_blend_rounded_rect(x+4, y, 22, 28, 3, RGB(220,228,244), 220);
        vbe_blend_rect(x+14, y, 12, 12, RGB(245,248,255), 180);
        vbe_blend_rect(x+16, y+14, 12, 2, RGB(180,190,215), 150);
        vbe_blend_rect(x+16, y+18, 12, 2, RGB(180,190,215), 150);
        vbe_blend_rect(x+16, y+22, 8, 2, RGB(180,190,215), 150);
    }
}

static void fm_draw(filemanager_t *fm) {
    if (!fm->win || !fm->win->visible) return;

    int wx = fm->win->x + 2;
    int wy = fm->win->y + TITLE_BAR_H;
    int ww = fm->win->w - 4;
    int wh = fm->win->h - TITLE_BAR_H - 4;

    vbe_blend_rect(wx, wy, ww, wh, RGB(245,248,255), 252);

    int tb_h = 44;
    vbe_blend_rect(wx, wy, ww, tb_h, RGB(250,252,255), 255);
    vbe_blend_rect(wx, wy+tb_h, ww, 1, RGB(210,218,238), 200);

    int btn_x = wx + 8;
    int btn_y = wy + 8;

    vbe_blend_rounded_rect(btn_x, btn_y, 28, 28, 8,
                           fm->hist_pos > 0 ? RGB(100,160,255) : RGB(200,208,228), 200);
    font_draw_string(btn_x+8, btn_y+7, "<", RGB(255,255,255), COLOR_TRANS, FONT_NORMAL);
    btn_x += 34;

    vbe_blend_rounded_rect(btn_x, btn_y, 28, 28, 8, RGB(200,208,228), 200);
    font_draw_string(btn_x+8, btn_y+7, ">", RGB(255,255,255), COLOR_TRANS, FONT_NORMAL);
    btn_x += 34;

    vbe_blend_rounded_rect(btn_x, btn_y, 28, 28, 8, RGB(200,208,228), 200);
    font_draw_string(btn_x+8, btn_y+7, "^", RGB(80,90,110), COLOR_TRANS, FONT_NORMAL);
    btn_x += 40;

    int addr_w = ww - btn_x + wx - 140;
    vbe_blend_rounded_rect(btn_x, btn_y, addr_w, 28, 6, RGB(255,255,255), 255);
    vbe_rounded_rect_outline(btn_x, btn_y, addr_w, 28, 6, 1, RGB(190,202,226));
    font_draw_string(btn_x + 8, btn_y + 7, fm->path, RGB(50,60,90), COLOR_TRANS, FONT_NORMAL);

    int hbx = btn_x + addr_w + 8;
    vbe_blend_rounded_rect(hbx, btn_y, 50, 28, 8, RGB(100,160,255), 220);
    font_draw_string(hbx+8, btn_y+7, "Home", RGB(255,255,255), COLOR_TRANS, FONT_SMALL);

    int sb_w = 160;
    vbe_blend_rect(wx, wy+tb_h, sb_w, wh - tb_h, RGB(248,250,255), 252);
    vbe_blend_rect(wx+sb_w, wy+tb_h, 1, wh-tb_h, RGB(210,218,238), 200);

    const char *side_items[] = {"Accueil", "Bureau", "Documents", "Images", "Télécharg.", "Musique", "Vidéos", NULL};
    const char *side_paths[] = {"/home", "/home/user/Desktop", "/home/user/Documents",
                                 "/home/user/Images", "/home/user/Downloads",
                                 "/home/user/Music", "/home/user/Videos", NULL};
    for (int i = 0; side_items[i]; i++) {
        int sy2 = wy + tb_h + 8 + i * 30;
        int is_active = (k_strcmp(fm->path, side_paths[i]) == 0);
        if (is_active) vbe_blend_rounded_rect(wx+4, sy2-2, sb_w-8, 26, 6, RGB(100,160,255), 40);
        font_draw_string(wx+12, sy2+6, side_items[i],
                         is_active ? RGB(60,120,220) : RGB(60,70,100),
                         COLOR_TRANS, FONT_NORMAL);
    }

    int fl_x = wx + sb_w + 4;
    int fl_y = wy + tb_h + 4;
    int fl_w = ww - sb_w - 6;
    int fl_h = wh - tb_h - 34;

    int row_h = 40;
    int rows_vis = fl_h / row_h;

    for (int i = 0; i < rows_vis; i++) {
        int idx = fm->scroll + i;
        if (idx >= fm->entry_count) break;
        int iy = fl_y + i * row_h;

        if (idx == fm->selected) {
            vbe_blend_rounded_rect(fl_x, iy, fl_w, row_h - 2, 6, RGB(100,160,255), 45);
            vbe_rounded_rect_outline(fl_x, iy, fl_w, row_h - 2, 6, 1, RGB(100,160,255));
        } else if (i % 2 == 0) {
            vbe_blend_rect(fl_x, iy, fl_w, row_h - 2, RGB(248,250,255), 180);
        }

        color_t icon_col = fm->is_dir[idx] ? RGB(255,190,60) : RGB(100,160,255);
        fm_draw_icon(fl_x + 6, iy + 4, fm->is_dir[idx], icon_col);

        const char *nm = fm->entries[idx];
        color_t name_col = idx == fm->selected ? RGB(30,60,120) : RGB(40,50,80);
        if (fm->is_dir[idx]) {
            char dn[FM_NAME_LEN + 2];
            fm_strcpy(dn, nm, FM_NAME_LEN);
            int dl = fm_strlen(dn);
            dn[dl] = '/'; dn[dl+1] = 0;
            font_draw_string(fl_x + 44, iy + 12, dn, name_col, COLOR_TRANS, FONT_NORMAL);
        } else {
            font_draw_string(fl_x + 44, iy + 12, nm, name_col, COLOR_TRANS, FONT_NORMAL);
        }
    }

    int st_y = fm->win->y + fm->win->h - 28;
    vbe_blend_rect(wx, st_y, ww, 26, RGB(240,244,252), 252);
    vbe_blend_rect(wx, st_y, ww, 1, RGB(210,218,238), 200);
    font_draw_string(wx+8, st_y+7, fm->status, RGB(80,90,120), COLOR_TRANS, FONT_SMALL);

    fm->need_redraw = 0;
}

static void fm_on_click(widget_t *w, gui_event_t *evt) {
    (void)w;
    filemanager_t *fm = &g_fm;
    if (evt->type == EVT_CLICK) {
        int mx = evt->x - fm->win->x;
        int my = evt->y - fm->win->y - TITLE_BAR_H;

        if (my >= 8 && my <= 36) {
            if (mx >= 8 && mx < 36) {
                if (fm->hist_pos > 0) {
                    fm->hist_pos--;
                    fm_load_dir(fm, fm->history[fm->hist_pos]);
                }
                return;
            }
            if (mx >= 42 && mx < 70) {
                if (fm->hist_pos < fm->hist_count - 1) {
                    fm->hist_pos++;
                    fm_load_dir(fm, fm->history[fm->hist_pos]);
                }
                return;
            }
            if (mx >= 76 && mx < 104) {
                if (k_strcmp(fm->path, "/") != 0) {
                    char parent[FM_PATH_LEN];
                    fm_strcpy(parent, fm->path, FM_PATH_LEN);
                    int len = fm_strlen(parent);
                    while (len > 1 && parent[len - 1] == '/') parent[--len] = 0;
                    while (len > 1 && parent[len - 1] != '/') parent[--len] = 0;
                    if (len > 1 && parent[len - 1] == '/') parent[len - 1] = 0;
                    if (!parent[0]) fm_strcpy(parent, "/", FM_PATH_LEN);
                    fm_navigate(fm, parent);
                }
                return;
            }
            int hbx = 8 + 34 + 34 + 40 + (fm->win->w - 4 - 8 - 34 - 34 - 40 - 140 - 8) - 8;
            if (mx >= hbx && mx < hbx + 50) {
                fm_navigate(fm, "/home");
                return;
            }
        }

        int tb_h = 44;
        int sb_w = 160;
        int fl_x = sb_w + 4;
        int fl_y = tb_h + 4;
        int row_h = 40;

        if (mx >= fl_x && my >= fl_y) {
            int idx = fm->scroll + (my - fl_y) / row_h;
            if (idx >= 0 && idx < fm->entry_count) {
                if (idx == fm->selected) {

                    if (fm->is_dir[idx]) {
                        char newpath[FM_PATH_LEN];
                        fm_strcpy(newpath, fm->path, FM_PATH_LEN);
                        int pl = fm_strlen(newpath);
                        if (pl > 1) { newpath[pl] = '/'; newpath[pl+1] = 0; }
                        int el = fm_strlen(fm->entries[idx]);
                        k_memcpy(newpath + fm_strlen(newpath), fm->entries[idx], el+1);
                        fm_navigate(fm, newpath);
                    } else {
                        gui_notify("Ouverture du fichier…");
                    }
                } else {
                    fm->selected = idx;
                    fm->need_redraw = 1;
                }
            }
        }

        const char *side_paths[] = {"/home", "/home/user/Desktop", "/home/user/Documents",
                                     "/home/user/Images", "/home/user/Downloads",
                                     "/home/user/Music", "/home/user/Videos", NULL};
        if (mx >= 0 && mx < sb_w + 4) {
            int siy = (my - tb_h - 8) / 30;
            if (siy >= 0 && side_paths[siy]) {
                fm_navigate(fm, side_paths[siy]);
            }
        }
    }

    if (evt->type == EVT_SCROLL) {
        int rows_vis = ((fm->win->h - TITLE_BAR_H - 8) - 44 - 4) / 40;
        int max_scroll = fm->entry_count - rows_vis;
        if (max_scroll < 0) max_scroll = 0;
        fm->scroll += evt->scroll > 0 ? -3 : 3;
        if (fm->scroll < 0) fm->scroll = 0;
        if (fm->scroll > max_scroll) fm->scroll = max_scroll;
        fm->need_redraw = 1;
        return;
    }

    if (evt->type == EVT_KEYDOWN) {
        key_event_t *k = &evt->key;
        if (k->scancode == KEY_UP && fm->selected > 0) {
            fm->selected--; if (fm->selected < fm->scroll) fm->scroll = fm->selected; fm->need_redraw = 1;
        }
        if (k->scancode == KEY_DOWN && fm->selected < fm->entry_count-1) {
            int rows_vis = ((fm->win->h - TITLE_BAR_H - 8) - 44 - 4) / 40;
            fm->selected++;
            if (fm->selected >= fm->scroll + rows_vis) fm->scroll = fm->selected - rows_vis + 1;
            fm->need_redraw = 1;
        }
    }
}

static void fm_paint(window_t *win) {
    (void)win;
    fm_draw(&g_fm);
}

void app_filemanager_open(void) {
    filemanager_t *fm = &g_fm;
    if (fm_open_count > 0) {
        if (fm->win) { gui_focus_window(fm->win); return; }
    }

    k_memset(fm, 0, sizeof(filemanager_t));
    fm_strcpy(fm->path, "/home", FM_PATH_LEN);

    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = sw * 3/4;
    int wh = sh * 3/4;
    int wx = (sw - ww)/2 - 20;
    int wy = (sh - wh)/2 + 20;

    window_t *win = gui_create_window(wx, wy, ww, wh, "Fichiers – NovaOS", WIN_DEFAULT);
    if (!win) return;
    win->bg_color = RGB(16,22,34);
    win->on_paint = fm_paint;
    fm->win = win;

    widget_t *mw = gui_add_label(win, 0, 0, win->w, win->h, "");
    if (mw) { mw->on_click = fm_on_click; mw->on_keydown = fm_on_click; }

    fm_navigate(fm, "/home");
    gui_show_window(win);
    gui_focus_window(win);
    fm_open_count++;
}
