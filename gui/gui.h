

#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include "../libc.h"
#include "../drivers/vbe.h"
#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "font.h"

#define MAX_WINDOWS   32
#define MAX_WIDGETS   64
#define TITLE_BAR_H   34
#define TASKBAR_H     56
#define BORDER_W       1
#define SHADOW_SIZE   12
#define ICON_SIZE     54
#define MENU_ITEM_H   36

typedef enum {
    EVT_NONE = 0,
    EVT_CLICK,
    EVT_RCLICK,
    EVT_DCLICK,
    EVT_MOUSEDOWN,
    EVT_MOUSEUP,
    EVT_MOUSEMOVE,
    EVT_KEYDOWN,
    EVT_KEYUP,
    EVT_CHAR,
    EVT_PAINT,
    EVT_CLOSE,
    EVT_RESIZE,
    EVT_FOCUS,
    EVT_BLUR,
    EVT_SCROLL,
    EVT_TIMER,
    EVT_HOVER,
    EVT_LEAVE
} event_type_t;

typedef struct {
    event_type_t type;
    int x, y;
    int dx, dy;
    uint8_t button;
    key_event_t key;
    void *target;
    int scroll;
} gui_event_t;

typedef enum {
    WIDGET_BUTTON = 0,
    WIDGET_LABEL,
    WIDGET_TEXTINPUT,
    WIDGET_CHECKBOX,
    WIDGET_RADIO,
    WIDGET_SLIDER,
    WIDGET_LISTBOX,
    WIDGET_SCROLLBAR,
    WIDGET_PROGRESSBAR,
    WIDGET_IMAGE,
    WIDGET_TEXTAREA,
    WIDGET_COMBO
} widget_type_t;

typedef struct window_t window_t;
typedef struct widget_t widget_t;

typedef void (*event_handler_t)(widget_t *widget, gui_event_t *evt);
typedef void (*paint_handler_t)(window_t *win);

struct widget_t {
    int x, y, w, h;
    widget_type_t type;
    char text[256];
    char placeholder[128];
    color_t bg, fg, border_color;
    color_t hover_color, active_color;
    int visible;
    int enabled;
    int focused;
    int hovered;
    int pressed;
    int checked;
    int value;
    int max_value;
    int min_value;
    int scroll_x, scroll_y;
    int cursor_pos;
    int sel_start, sel_end;
    char **list_items;
    int list_count;
    int list_selected;
    event_handler_t on_click;
    event_handler_t on_change;
    event_handler_t on_keydown;
    event_handler_t on_hover;
    void *userdata;
    window_t *window;
    uint32_t *icon_data;
    int icon_w, icon_h;
};

#define WIN_CLOSABLE    0x01
#define WIN_RESIZABLE   0x02
#define WIN_MINIMIZABLE 0x04
#define WIN_MAXIMIZABLE 0x08
#define WIN_MOVABLE     0x10
#define WIN_MODAL       0x20
#define WIN_SHADOW      0x40
#define WIN_TOPMOST     0x80
#define WIN_BORDERLESS  0x100
#define WIN_TRANSPARENT 0x200
#define WIN_DEFAULT     (WIN_CLOSABLE|WIN_MINIMIZABLE|WIN_MAXIMIZABLE|WIN_MOVABLE|WIN_SHADOW)

typedef enum {
    WIN_STATE_NORMAL = 0,
    WIN_STATE_MINIMIZED,
    WIN_STATE_MAXIMIZED
} win_state_t;

struct window_t {
    int x, y, w, h;
    int save_x, save_y, save_w, save_h;
    int min_w, min_h;
    char title[128];
    uint32_t flags;
    win_state_t state;
    int visible;
    int focused;
    int dragging;
    int drag_ox, drag_oy;
    int resizing;
    int zorder;
    color_t bg_color;
    color_t title_color;
    color_t title_text_color;
    widget_t widgets[MAX_WIDGETS];
    int widget_count;
    paint_handler_t on_paint;
    event_handler_t on_event;
    void *userdata;
    uint32_t *buffer;
    int needs_redraw;
    int id;

    char taskbar_name[32];
    uint32_t *taskbar_icon;
    int close_btn_hover;
    int min_btn_hover;
    int max_btn_hover;
};

typedef struct {
    int x, y;
    char name[64];
    uint32_t *icon;
    void (*on_open)(void);
    int launch_mode;
    char target[256];
    int hovered;
    int selected;
} desktop_icon_t;

typedef struct {
    window_t windows[MAX_WINDOWS];
    int window_count;
    int focused_window;
    int next_zorder;
    int next_id;

    color_t desktop_bg;
    desktop_icon_t icons[32];
    int icon_count;

    int cursor_x, cursor_y;
    uint8_t cursor_down;
    uint8_t cursor_prev_down;
    uint8_t cursor_right_down;
    uint8_t cursor_prev_right_down;

    int drag_win;
    int drag_ox, drag_oy;

    int context_menu_visible;
    int context_menu_x, context_menu_y;

    int taskbar_start_open;
    int taskbar_start_hovered;

    int hour, min, sec;

    char notif_text[128];
    uint32_t notif_time;
} gui_state_t;

extern gui_state_t gui;
extern int gui_wallpaper_variant;
extern int gui_lockscreen_variant;
extern int gui_accent_variant;
extern int gui_taskbar_variant;
void gui_activate_lockscreen(void);
void gui_preferences_load(void);
void gui_preferences_save(void);

void gui_init(void);
void gui_main_loop(void);
void gui_redraw_all(void);
void gui_draw_desktop(void);
void gui_draw_taskbar(void);
void gui_draw_cursor(void);

window_t* gui_create_window(int x, int y, int w, int h, const char *title, uint32_t flags);
void gui_destroy_window(window_t *win);
void gui_show_window(window_t *win);
void gui_hide_window(window_t *win);
void gui_focus_window(window_t *win);
void gui_draw_window(window_t *win);
void gui_close_window(window_t *win);
void gui_minimize_window(window_t *win);
void gui_maximize_window(window_t *win);
void gui_restore_window(window_t *win);

widget_t* gui_add_widget(window_t *win, widget_type_t type, int x, int y, int w, int h, const char *text);
widget_t* gui_add_button(window_t *win, int x, int y, int w, int h, const char *text, event_handler_t cb);
widget_t* gui_add_label(window_t *win, int x, int y, int w, int h, const char *text);
widget_t* gui_add_textinput(window_t *win, int x, int y, int w, int h, const char *placeholder);
widget_t* gui_add_textarea(window_t *win, int x, int y, int w, int h);
widget_t* gui_add_listbox(window_t *win, int x, int y, int w, int h);
widget_t* gui_add_checkbox(window_t *win, int x, int y, const char *text);
widget_t* gui_add_progressbar(window_t *win, int x, int y, int w, int h);
widget_t* gui_add_slider(window_t *win, int x, int y, int w, int h, int min, int max, int val);

void gui_draw_widget(widget_t *widget);
void gui_draw_button(widget_t *w);
void gui_draw_label(widget_t *w);
void gui_draw_textinput(widget_t *w);
void gui_draw_textarea(widget_t *w);
void gui_draw_listbox(widget_t *w);
void gui_draw_checkbox(widget_t *w);
void gui_draw_progressbar(widget_t *w);
void gui_draw_slider(widget_t *w);

void gui_handle_mouse(mouse_state_t *state);
void gui_handle_key(key_event_t *evt);
void gui_dispatch_event(window_t *win, gui_event_t *evt);

void gui_add_desktop_icon(int x, int y, const char *name, void (*on_open)(void));
void gui_add_desktop_icon_link(int x, int y, const char *name, const char *target);
void gui_draw_desktop_icon(desktop_icon_t *icon);
void gui_refresh_shortcuts(void);

void gui_notify(const char *msg);
void gui_msgbox(const char *title, const char *msg);
void gui_show_context_menu(int x, int y);
void gui_string_copy(char *dst, const char *src, int max);
int  gui_string_len(const char *s);
void gui_int_to_str(int n, char *buf);

void app_terminal_open(void);
void app_editor_open(void);
void app_filemanager_open(void);
void app_calculator_open(void);
void app_browser_open(void);
void app_browser_open_url(const char *url);
void app_settings_open(void);
void app_clock_open(void);
void app_system_monitor_open(void);
void app_userspace_open(void);
void app_about_open(void);
void app_installer_open(void);
void app_tutorial_open(void);
void app_nova_hub_open(void);
void app_quick_notes_open(void);
void app_symera_open(void);

#endif
