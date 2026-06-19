#ifndef CUSTOM_ASSETS_H
#define CUSTOM_ASSETS_H

#include <stdint.h>
#include "../drivers/vbe.h"

typedef struct {
    const uint32_t *pixels;
    int w;
    int h;
} ui_bitmap_t;

const ui_bitmap_t *ui_bitmap_for_name(const char *name);
const ui_bitmap_t *ui_bitmap_for_window_title(const char *title);
const ui_bitmap_t *ui_bitmap_wallpaper(void);
const ui_bitmap_t *ui_bitmap_start_button(void);
void ui_draw_bitmap_scaled(const ui_bitmap_t *bmp, int x, int y, int w, int h);

#endif
