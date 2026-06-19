

#include "../gui/gui.h"
#include "../gui/font.h"
#include "../drivers/vbe.h"
#include "../drivers/keyboard.h"
#include "../kernel/timer.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

#define SCALE 1000000LL

typedef struct {
    int64_t value;
    int64_t operand;
    char    op;
    char    display[64];
    int     fresh;
    window_t *win;
    int     need_redraw;
    int     error;
    int     has_decimal;
    int     decimal_digits;
} calculator_t;

static calculator_t g_calc;
static int calc_open = 0;

static int calc_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }

static void fp_to_str(int64_t v, char *buf, int maxlen) {
    k_memset(buf, 0, maxlen);
    int pos = 0;
    if (v < 0) { buf[pos++] = '-'; v = -v; }

    int64_t ipart = v / SCALE;
    int64_t fpart = v % SCALE;

    if (ipart == 0) {
        buf[pos++] = '0';
    } else {
        char ibuf[24]; int ilen = 0;
        int64_t tmp = ipart;
        while (tmp > 0 && ilen < 18) { ibuf[ilen++] = '0' + (int)(tmp % 10); tmp /= 10; }
        for (int i = ilen-1; i >= 0 && pos < maxlen-1; i--) buf[pos++] = ibuf[i];
    }

    if (fpart > 0) {
        buf[pos++] = '.';
        char fbuf[8]; int flen = 0;
        int64_t tmp = fpart;

        for (int i = 0; i < 6; i++) {
            fbuf[flen++] = '0' + (int)(tmp / (SCALE/10));
            tmp = (tmp % (SCALE/10)) * 10;
        }

        while (flen > 0 && fbuf[flen-1] == '0') flen--;
        for (int i = 0; i < flen && pos < maxlen-1; i++) buf[pos++] = fbuf[i];
    }
    buf[pos] = 0;
}

static int64_t str_to_fp(const char *s) {
    int64_t result = 0;
    int neg = 0, i = 0;
    if (s[0] == '-') { neg = 1; i++; }

    for (; s[i] && s[i] != '.'; i++) {
        if (s[i] >= '0' && s[i] <= '9')
            result = result * 10 + (s[i]-'0');
    }
    result *= SCALE;

    if (s[i] == '.') {
        i++;
        int64_t fscale = SCALE / 10;
        for (; s[i] && fscale > 0; i++) {
            if (s[i] >= '0' && s[i] <= '9') {
                result += (s[i]-'0') * fscale;
                fscale /= 10;
            }
        }
    }
    return neg ? -result : result;
}

static void calc_display_update(calculator_t *c) {
    c->need_redraw = 1;
}

static void calc_press_digit(calculator_t *c, char d) {
    if (c->error) { k_memset(c->display, 0, 64); c->display[0]='0'; c->error=0; c->has_decimal=0; c->decimal_digits=0; }
    if (c->fresh) { k_memset(c->display, 0, 64); c->display[0]='0'; c->fresh=0; c->has_decimal=0; c->decimal_digits=0; }
    int len = calc_strlen(c->display);
    if (len >= 14) return;
    if (c->has_decimal) {
        c->decimal_digits++;
        if (c->decimal_digits > 6) return;
        c->display[len] = d; c->display[len+1] = 0;
    } else {
        if (len == 1 && c->display[0] == '0') { c->display[0] = d; c->display[1] = 0; }
        else { c->display[len] = d; c->display[len+1] = 0; }
    }
    calc_display_update(c);
}

static void calc_press_dot(calculator_t *c) {
    if (c->has_decimal) return;
    if (c->fresh) { k_memcpy(c->display, "0.", 3); c->fresh=0; c->has_decimal=1; c->decimal_digits=0; return; }
    int len = calc_strlen(c->display);
    if (len < 13) { c->display[len]='.'; c->display[len+1]=0; }
    c->has_decimal = 1; c->decimal_digits = 0;
    calc_display_update(c);
}

static int64_t calc_apply(int64_t a, int64_t b, char op) {
    switch(op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return (a / (SCALE/1000)) * (b / 1000);
        case '/': return (b != 0) ? (a / (b/SCALE)) : 0;
        default: return b;
    }
}

static void calc_press_op(calculator_t *c, char op) {
    int64_t cur = str_to_fp(c->display);
    if (c->op && !c->fresh) {
        int64_t result = calc_apply(c->operand, cur, c->op);
        fp_to_str(result, c->display, 64);
        c->value = result;
    } else {
        c->value = cur;
    }
    c->operand = c->value;
    c->op = op;
    c->fresh = 1;
    c->has_decimal = 0; c->decimal_digits = 0;
    calc_display_update(c);
}

static void calc_press_equals(calculator_t *c) {
    int64_t cur = str_to_fp(c->display);
    if (c->op) {
        if (c->op == '/' && cur == 0) {
            k_memcpy(c->display, "Erreur", 7); c->error = 1;
        } else {
            int64_t result = calc_apply(c->operand, cur, c->op);
            fp_to_str(result, c->display, 64);
            c->value = result;
        }
    }
    c->op = 0; c->fresh = 1;
    c->has_decimal = 0; c->decimal_digits = 0;
    calc_display_update(c);
}

static void calc_press_clear(calculator_t *c) {
    k_memset(c->display, 0, 64);
    c->display[0] = '0'; c->display[1] = 0;
    c->op = 0; c->operand = 0; c->value = 0;
    c->fresh = 0; c->error = 0; c->has_decimal = 0; c->decimal_digits = 0;
    calc_display_update(c);
}

static void calc_press_backspace(calculator_t *c) {
    int len = calc_strlen(c->display);
    if (len > 1) {
        if (c->display[len-1] == '.') c->has_decimal = 0;
        c->display[len-1] = 0;
    } else {
        c->display[0] = '0';
        c->has_decimal = 0; c->decimal_digits = 0;
    }
    calc_display_update(c);
}

static void calc_press_negate(calculator_t *c) {
    if (c->display[0] == '-') {
        k_memcpy(c->display, c->display+1, 63);
    } else if (c->display[0] != '0') {
        int len = calc_strlen(c->display);
        for (int i = len; i >= 0; i--) c->display[i+1] = c->display[i];
        c->display[0] = '-';
    }
    calc_display_update(c);
}

static void calc_percent(calculator_t *c) {
    int64_t cur = str_to_fp(c->display);
    int64_t result = cur / 100;
    fp_to_str(result, c->display, 64);
    calc_display_update(c);
}

static void calc_draw(calculator_t *c) {
    if (!c->win || !c->win->visible) return;
    int wx = c->win->x + 4;
    int wy = c->win->y + TITLE_BAR_H + 2;
    int ww = c->win->w - 8;
    int wh = c->win->h - TITLE_BAR_H - 6;

    vbe_blend_rounded_rect(wx, wy, ww, wh, 12, RGB(30,34,46), 252);

    int disp_h = 80;
    vbe_blend_rounded_rect(wx+4, wy+8, ww-8, disp_h, 8, RGB(18,22,32), 255);
    vbe_rounded_rect_outline(wx+4, wy+8, ww-8, disp_h, 8, 1, RGB(60,80,120));

    const char *disp = c->display[0] ? c->display : "0";
    int dl = calc_strlen(disp);
    int dw = dl * 12;
    int dx = wx + 4 + (ww-8) - dw - 16;
    if (dx < wx + 8) dx = wx + 8;
    font_draw_string(dx, wy + 28, disp,
                     c->error ? RGB(255,80,80) : RGB(220,240,255),
                     COLOR_TRANS, FONT_LARGE);

    if (c->op) {
        char op_str[3] = {c->op, 0, 0};
        font_draw_string(wx + 12, wy + 20, op_str, RGB(100,180,255), COLOR_TRANS, FONT_NORMAL);
    }

    struct { const char *lbl; int gx, gy; color_t col; char act; char val; } btns[] = {
        {"AC",  0,0, RGB(220,60,60),  'C', 0},
        {"+/-", 1,0, RGB(80,90,110),  'N', 0},
        {"%",   2,0, RGB(80,90,110),  '%', 0},
        {"÷",   3,0, RGB(255,150,50), '/', 0},
        {"7",   0,1, RGB(50,56,78),   '0','7'},
        {"8",   1,1, RGB(50,56,78),   '0','8'},
        {"9",   2,1, RGB(50,56,78),   '0','9'},
        {"×",   3,1, RGB(255,150,50), '*', 0},
        {"4",   0,2, RGB(50,56,78),   '0','4'},
        {"5",   1,2, RGB(50,56,78),   '0','5'},
        {"6",   2,2, RGB(50,56,78),   '0','6'},
        {"-",   3,2, RGB(255,150,50), '-', 0},
        {"1",   0,3, RGB(50,56,78),   '0','1'},
        {"2",   1,3, RGB(50,56,78),   '0','2'},
        {"3",   2,3, RGB(50,56,78),   '0','3'},
        {"+",   3,3, RGB(255,150,50), '+', 0},
        {"⌫",   0,4, RGB(60,70,95),   'B', 0},
        {"0",   1,4, RGB(50,56,78),   '0','0'},
        {".",   2,4, RGB(50,56,78),   '.', 0},
        {"=",   3,4, RGB(80,180,100), '=', 0},
        {NULL,0,0,0,0,0}
    };

    int btn_area_y = wy + disp_h + 16;
    int btn_area_h = wh - disp_h - 20;
    int btn_w = (ww - 12) / 4;
    int btn_h = btn_area_h / 5;

    for (int i = 0; btns[i].lbl; i++) {
        int bx = wx + 4 + btns[i].gx * btn_w;
        int by = btn_area_y + btns[i].gy * btn_h;
        int bw2 = btn_w - 4;
        int bh2 = btn_h - 4;
        vbe_blend_rounded_rect(bx+2, by+2, bw2, bh2, 8, btns[i].col, 240);
        int tw = calc_strlen(btns[i].lbl) * 8;
        int tx = bx + 2 + (bw2 - tw) / 2;
        int ty = by + 2 + (bh2 - 16) / 2;
        font_draw_string(tx, ty, btns[i].lbl, COLOR_WHITE, COLOR_TRANS, FONT_LARGE);
    }

    c->need_redraw = 0;
}

static void calc_on_click(widget_t *w, gui_event_t *evt) {
    (void)w;
    calculator_t *c = &g_calc;
    if (evt->type == EVT_CLICK) {
        int mx = evt->x - c->win->x - 4;
        int my = evt->y - c->win->y - TITLE_BAR_H - 2;
        int ww = c->win->w - 8;
        int wh = c->win->h - TITLE_BAR_H - 6;
        int disp_h = 80;
        int btn_area_y = disp_h + 16;
        int btn_w = (ww - 12) / 4;
        int btn_h = (wh - disp_h - 20) / 5;

        if (my < btn_area_y) return;
        int gy = (my - btn_area_y) / btn_h;
        int gx = mx / btn_w;
        if (gx < 0 || gx > 3 || gy < 0 || gy > 4) return;

        struct { int gx, gy; char act; char val; } map[] = {
            {0,0,'C',0},{1,0,'N',0},{2,0,'%',0},{3,0,'/',0},
            {0,1,'0','7'},{1,1,'0','8'},{2,1,'0','9'},{3,1,'*',0},
            {0,2,'0','4'},{1,2,'0','5'},{2,2,'0','6'},{3,2,'-',0},
            {0,3,'0','1'},{1,3,'0','2'},{2,3,'0','3'},{3,3,'+',0},
            {0,4,'B',0},{1,4,'0','0'},{2,4,'.',0},{3,4,'=',0},
            {-1,0,0,0}
        };
        for (int i = 0; map[i].gx >= 0; i++) {
            if (map[i].gx == gx && map[i].gy == gy) {
                switch(map[i].act) {
                    case '0': calc_press_digit(c, map[i].val); break;
                    case 'C': calc_press_clear(c); break;
                    case 'B': calc_press_backspace(c); break;
                    case 'N': calc_press_negate(c); break;
                    case '%': calc_percent(c); break;
                    case '.': calc_press_dot(c); break;
                    case '=': calc_press_equals(c); break;
                    case '+': calc_press_op(c, '+'); break;
                    case '-': calc_press_op(c, '-'); break;
                    case '*': calc_press_op(c, '*'); break;
                    case '/': calc_press_op(c, '/'); break;
                }
                break;
            }
        }
    }
    if (evt->type == EVT_KEYDOWN || evt->type == EVT_CHAR) {
        key_event_t *k = &evt->key;
        if (k->released) return;
        if (k->ascii >= '0' && k->ascii <= '9') calc_press_digit(c, k->ascii);
        else if (k->ascii == '.') calc_press_dot(c);
        else if (k->ascii == '+') calc_press_op(c, '+');
        else if (k->ascii == '-') calc_press_op(c, '-');
        else if (k->ascii == '*') calc_press_op(c, '*');
        else if (k->ascii == '/') calc_press_op(c, '/');
        else if (k->ascii == '=') calc_press_equals(c);
        else if (k->scancode == KEY_ENTER) calc_press_equals(c);
        else if (k->scancode == KEY_BACKSPACE) calc_press_backspace(c);
        else if (k->ascii == 'c' || k->ascii == 'C') calc_press_clear(c);
    }
}

static void calc_paint(window_t *win) {
    (void)win;
    calc_draw(&g_calc);
}

void app_calculator_open(void) {
    calculator_t *c = &g_calc;
    if (calc_open) {
        if (c->win) { gui_focus_window(c->win); return; }
    }

    k_memset(c, 0, sizeof(calculator_t));
    c->display[0] = '0'; c->display[1] = 0;

    int sw = vbe.width ? (int)vbe.width : 1920;
    int sh = vbe.height ? (int)vbe.height : 1080;
    int ww = 320, wh = 520;
    int wx = (sw - ww)/2 + 100;
    int wy = (sh - wh)/2;

    window_t *win = gui_create_window(wx, wy, ww, wh, "Calculatrice", WIN_DEFAULT);
    if (!win) return;
    win->bg_color = RGB(30,34,46);
    win->on_paint = calc_paint;
    c->win = win;

    widget_t *mw = gui_add_label(win, 0, 0, ww, wh, "");
    if (mw) { mw->on_click = calc_on_click; mw->on_keydown = calc_on_click; }

    gui_show_window(win);
    gui_focus_window(win);
    calc_open = 1;
}
