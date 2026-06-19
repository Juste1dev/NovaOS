

#include "../gui/gui.h"
#include "../gui/font.h"
#include "../drivers/vbe.h"
#include "../drivers/keyboard.h"
#include "../kernel/timer.h"
#include "../kernel/memory.h"
#include "../fs/vfs.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

#define ED_MAX_LINES  1024
#define ED_LINE_W     256
#define ED_MAX_PATH   256

typedef struct {
    char   lines[ED_MAX_LINES][ED_LINE_W];
    int    line_count;
    int    cur_line;
    int    cur_col;
    int    scroll_y;
    char   filepath[ED_MAX_PATH];
    int    modified;
    int    selection;
    int    sel_line, sel_col;
    window_t *win;
    widget_t *statusbar;
    int    need_redraw;
    int    blink;
    uint32_t blink_time;
    int    show_find;
    char   find_buf[128];
    int    find_len;
} editor_t;

static editor_t g_editor;
static int editor_open_count = 0;

static int ed_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }
static void ed_strcpy(char *d, const char *s, int max) {
    int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0;
}
static void ed_itoa(int n, char *buf) {
    if (n < 0) { *buf++ = '-'; n = -n; }
    char tmp[12]; int len=0;
    if (n==0){tmp[len++]='0';}
    while(n>0){tmp[len++]='0'+(n%10);n/=10;}
    for(int i=len-1;i>=0;i--)*buf++=tmp[i];
    *buf=0;
}

static void ed_insert_char(editor_t *e, char c) {
    if (e->cur_line >= ED_MAX_LINES) return;
    char *line = e->lines[e->cur_line];
    int len = ed_strlen(line);
    if (len >= ED_LINE_W - 2) return;

    for (int i = len; i > e->cur_col; i--) line[i] = line[i-1];
    line[e->cur_col] = c;
    line[len+1] = 0;
    e->cur_col++;
    e->modified = 1;
    e->need_redraw = 1;
}

static void ed_delete_char(editor_t *e) {
    if (e->cur_col == 0) {
        if (e->cur_line == 0) return;

        char *prev = e->lines[e->cur_line - 1];
        char *cur  = e->lines[e->cur_line];
        int prev_len = ed_strlen(prev);
        int cur_len  = ed_strlen(cur);
        if (prev_len + cur_len < ED_LINE_W - 1) {
            k_memcpy(prev + prev_len, cur, cur_len + 1);

            for (int i = e->cur_line; i < e->line_count - 1; i++)
                k_memcpy(e->lines[i], e->lines[i+1], ED_LINE_W);
            k_memset(e->lines[e->line_count - 1], 0, ED_LINE_W);
            e->line_count--;
            e->cur_line--;
            e->cur_col = prev_len;
        }
    } else {
        char *line = e->lines[e->cur_line];
        int len = ed_strlen(line);
        for (int i = e->cur_col - 1; i < len - 1; i++) line[i] = line[i+1];
        line[len-1] = 0;
        e->cur_col--;
    }
    e->modified = 1;
    e->need_redraw = 1;
}

static void ed_newline(editor_t *e) {
    if (e->line_count >= ED_MAX_LINES - 1) return;

    for (int i = e->line_count; i > e->cur_line + 1; i--)
        k_memcpy(e->lines[i], e->lines[i-1], ED_LINE_W);
    e->line_count++;

    char *cur = e->lines[e->cur_line];
    char *next = e->lines[e->cur_line + 1];
    int split_at = e->cur_col;
    ed_strcpy(next, cur + split_at, ED_LINE_W);
    cur[split_at] = 0;
    e->cur_line++;
    e->cur_col = 0;
    e->modified = 1;
    e->need_redraw = 1;
}

static void ed_save(editor_t *e) {
    if (!e->filepath[0]) return;

    static char save_buf[ED_MAX_LINES * ED_LINE_W];
    int pos = 0;
    for (int i = 0; i < e->line_count && pos < (int)sizeof(save_buf) - 2; i++) {
        int len = ed_strlen(e->lines[i]);
        k_memcpy(save_buf + pos, e->lines[i], len);
        pos += len;
        save_buf[pos++] = '\n';
    }
    save_buf[pos] = 0;
    vfs_write_file(e->filepath, save_buf, pos);
    e->modified = 0;
    gui_notify("Fichier sauvegardé !");
    e->need_redraw = 1;
}

static void ed_load(editor_t *e, const char *path) {
    static char load_buf[ED_MAX_LINES * ED_LINE_W];
    vfs_get_contents(path, load_buf, sizeof(load_buf)-1); int sz = (int)vfs_get_size(path);
    if (sz < 0) return;
    load_buf[sz] = 0;
    ed_strcpy(e->filepath, path, ED_MAX_PATH);
    e->line_count = 0;
    e->cur_line = 0; e->cur_col = 0; e->scroll_y = 0;

    int lstart = 0;
    for (int i = 0; i <= sz && e->line_count < ED_MAX_LINES-1; i++) {
        if (load_buf[i] == '\n' || load_buf[i] == 0) {
            int len = i - lstart;
            if (len >= ED_LINE_W) len = ED_LINE_W - 1;
            k_memcpy(e->lines[e->line_count], load_buf + lstart, len);
            e->lines[e->line_count][len] = 0;
            e->line_count++;
            lstart = i + 1;
        }
    }
    if (e->line_count == 0) { e->line_count = 1; }
    e->modified = 0;
    e->need_redraw = 1;
}

static void ed_update_title(editor_t *e) {
    if (!e->win) return;
    char title[200];
    k_memset(title, 0, sizeof(title));
    char buf2[200];
    k_memset(buf2, 0, sizeof(buf2));
    if (e->filepath[0]) {

        const char *bn = e->filepath;
        for (int i = 0; e->filepath[i]; i++)
            if (e->filepath[i] == '/') bn = &e->filepath[i+1];
        k_memcpy(title, bn, 100);
    } else {
        k_memcpy(title, "Sans titre", 11);
    }
    k_memcpy(buf2, title, 100);
    if (e->modified) {
        int l2 = ed_strlen(buf2);
        buf2[l2] = ' '; buf2[l2+1]='*'; buf2[l2+2]=0;
    }
    char full[200]; k_memset(full, 0, sizeof(full));
    k_memcpy(full, "Éditeur – ", 11);
    int fl = ed_strlen(full);
    k_memcpy(full+fl, buf2, 100);
    k_memcpy(e->win->title, full, 127);
}

static void ed_draw(editor_t *e) {
    if (!e->win || !e->win->visible) return;
    int wx = e->win->x + 2;
    int wy = e->win->y + TITLE_BAR_H;
    int ww = e->win->w - 4;
    int wh = e->win->h - TITLE_BAR_H - 28;

    vbe_blend_rect(wx, wy, ww, wh, RGB(24,28,40), 248);

    int char_w = 8, char_h = 16;
    int rows_vis = wh / char_h - 1;
    int cols_vis = (ww - 60) / char_w;

    vbe_blend_rect(wx, wy, 55, wh, RGB(18,22,34), 248);
    vbe_blend_rect(wx+55, wy, 1, wh, RGB(60,70,100), 200);

    if (e->cur_line < e->scroll_y) e->scroll_y = e->cur_line;
    if (e->cur_line >= e->scroll_y + rows_vis) e->scroll_y = e->cur_line - rows_vis + 1;

    for (int r = 0; r < rows_vis; r++) {
        int src_line = e->scroll_y + r;
        if (src_line >= e->line_count) break;
        int py = wy + 4 + r * char_h;

        if (src_line == e->cur_line) {
            vbe_blend_rect(wx+56, py-1, ww-56, char_h+1, RGB(36,44,64), 200);
        }

        char lnum[8]; ed_itoa(src_line + 1, lnum);
        int numw = ed_strlen(lnum) * char_w;
        font_draw_string(wx + 50 - numw, py, lnum,
                         src_line == e->cur_line ? RGB(180,190,220) : RGB(80,90,120),
                         COLOR_TRANS, FONT_NORMAL);

        const char *line = e->lines[src_line];
        int len = ed_strlen(line);
        int col_offset = 0;
        for (int c = col_offset; c < len && (c - col_offset) < cols_vis; c++) {
            int px = wx + 58 + (c - col_offset) * char_w;
            char ch = line[c];
            color_t fc;

            if (ch == '/' || ch == '*' || ch == '#') fc = RGB(100,180,100);
            else if (ch == '"' || ch == '\'')  fc = RGB(255,180,80);
            else if (ch >= '0' && ch <= '9')   fc = RGB(180,140,255);
            else fc = RGB(210,220,240);
            font_draw_char(px, py, ch, fc, COLOR_TRANS, FONT_NORMAL);
        }
    }

    uint32_t now = timer_ms();
    if (now - e->blink_time > 530) {
        e->blink = !e->blink;
        e->blink_time = now;
    }
    if (e->blink && e->cur_line >= e->scroll_y && e->cur_line < e->scroll_y + rows_vis) {
        int r2 = e->cur_line - e->scroll_y;
        int py = wy + 4 + r2 * char_h;
        int cx = wx + 58 + e->cur_col * char_w;
        vbe_blend_rect(cx, py, 2, char_h, RGB(200,220,255), 220);
    }

    int sy = e->win->y + e->win->h - 26;
    vbe_blend_rect(wx, sy, ww, 24, RGB(18,22,34), 248);
    vbe_blend_rect(wx, sy, ww, 1, RGB(60,70,100), 200);

    char status[256]; k_memset(status, 0, sizeof(status));
    char nb[16];
    k_memcpy(status, "Ln ", 4);
    ed_itoa(e->cur_line+1, nb); int sl = ed_strlen(status);
    k_memcpy(status+sl, nb, 8); sl = ed_strlen(status);
    k_memcpy(status+sl, ", Col ", 7); sl = ed_strlen(status);
    ed_itoa(e->cur_col+1, nb);
    k_memcpy(status+sl, nb, 8); sl = ed_strlen(status);
    k_memcpy(status+sl, "  |  ", 6); sl = ed_strlen(status);
    ed_itoa(e->line_count, nb);
    k_memcpy(status+sl, nb, 8); sl = ed_strlen(status);
    k_memcpy(status+sl, " lignes", 8); sl = ed_strlen(status);
    if (e->modified) { k_memcpy(status+sl, "  [modifié]", 12); sl = ed_strlen(status); }
    k_memcpy(status+sl, "   Ctrl+S: Sauvegarder  Ctrl+N: Nouveau  Ctrl+O: Ouvrir", 60);
    font_draw_string(wx + 8, sy + 5, status, RGB(140,160,200), COLOR_TRANS, FONT_SMALL);

    e->need_redraw = 0;
}

static void ed_on_key(widget_t *w, gui_event_t *evt) {
    (void)w;
    editor_t *e = &g_editor;
    if (evt->type != EVT_KEYDOWN && evt->type != EVT_CHAR) return;
    key_event_t *k = &evt->key;
    if (k->released) return;

    if (k->ctrl) {
        if (k->ascii == 's' || k->ascii == 'S') { ed_save(e); return; }
        if (k->ascii == 'n' || k->ascii == 'N') {
            k_memset(e->lines, 0, sizeof(e->lines));
            e->line_count = 1; e->cur_line = 0; e->cur_col = 0;
            k_memset(e->filepath, 0, sizeof(e->filepath));
            e->modified = 0; e->need_redraw = 1;
            ed_update_title(e);
            return;
        }
        if (k->ascii == 'a' || k->ascii == 'A') {

            e->cur_line = e->line_count - 1;
            e->cur_col = ed_strlen(e->lines[e->cur_line]);
            e->need_redraw = 1;
            return;
        }
    }

    if (k->scancode == KEY_ENTER) {
        ed_newline(e);
    } else if (k->scancode == KEY_BACKSPACE) {
        ed_delete_char(e);
    } else if (k->scancode == KEY_LEFT) {
        if (e->cur_col > 0) e->cur_col--;
        else if (e->cur_line > 0) { e->cur_line--; e->cur_col = ed_strlen(e->lines[e->cur_line]); }
        e->need_redraw = 1;
    } else if (k->scancode == KEY_RIGHT) {
        int len = ed_strlen(e->lines[e->cur_line]);
        if (e->cur_col < len) e->cur_col++;
        else if (e->cur_line < e->line_count - 1) { e->cur_line++; e->cur_col = 0; }
        e->need_redraw = 1;
    } else if (k->scancode == KEY_UP) {
        if (e->cur_line > 0) {
            e->cur_line--;
            int len2 = ed_strlen(e->lines[e->cur_line]);
            if (e->cur_col > len2) e->cur_col = len2;
        }
        e->need_redraw = 1;
    } else if (k->scancode == KEY_DOWN) {
        if (e->cur_line < e->line_count - 1) {
            e->cur_line++;
            int len2 = ed_strlen(e->lines[e->cur_line]);
            if (e->cur_col > len2) e->cur_col = len2;
        }
        e->need_redraw = 1;
    } else if (k->scancode == KEY_HOME) {
        e->cur_col = 0; e->need_redraw = 1;
    } else if (k->scancode == KEY_END) {
        e->cur_col = ed_strlen(e->lines[e->cur_line]); e->need_redraw = 1;
    } else if (k->scancode == KEY_PGUP) {
        e->cur_line -= 20; if (e->cur_line < 0) e->cur_line = 0;
        e->need_redraw = 1;
    } else if (k->scancode == KEY_PGDN) {
        e->cur_line += 20; if (e->cur_line >= e->line_count) e->cur_line = e->line_count-1;
        e->need_redraw = 1;
    } else if (k->scancode == KEY_DEL) {

        char *line = e->lines[e->cur_line];
        int len = ed_strlen(line);
        if (e->cur_col < len) {
            for (int i = e->cur_col; i < len; i++) line[i] = line[i+1];
            e->modified = 1; e->need_redraw = 1;
        }
    } else if (k->ascii >= 32 && k->ascii < 127) {
        ed_insert_char(e, k->ascii);
    } else if (k->ascii == '\t') {

        for (int i = 0; i < 4; i++) ed_insert_char(e, ' ');
    }
    ed_update_title(e);
}

static void ed_paint(window_t *win) {
    (void)win;
    ed_draw(&g_editor);
}

void app_editor_open(void) {
    editor_t *e = &g_editor;
    if (editor_open_count > 0) {
        if (e->win) { gui_focus_window(e->win); return; }
    }

    k_memset(e, 0, sizeof(editor_t));
    e->line_count = 1;
    e->blink_time = timer_ms();

    e->lines[0][0] = 0;
    e->lines[1][0] = 0;
    e->line_count = 2;

    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = sw * 3 / 4;
    int wh = sh * 3 / 4;
    int wx = (sw - ww) / 2 + 40;
    int wy = (sh - wh) / 2;

    window_t *win = gui_create_window(wx, wy, ww, wh, "Éditeur – Sans titre", WIN_DEFAULT);
    if (!win) return;
    win->bg_color = RGB(24,28,40);
    win->on_paint = ed_paint;
    e->win = win;

    widget_t *kw = gui_add_label(win, 0, 0, 1, 1, "");
    if (kw) kw->on_keydown = ed_on_key;

    gui_show_window(win);
    gui_focus_window(win);
    editor_open_count++;
}
