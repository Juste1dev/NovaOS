

#ifndef DESKTOPS_FONT_H
#define DESKTOPS_FONT_H

#include <stdint.h>
#include "../drivers/vbe.h"

#define FONT_W    8
#define FONT_H    16
#define FONT_SM_W 7
#define FONT_SM_H 12
#define FONT_LG_W 12
#define FONT_LG_H 24
#define FONT_TITLE_W 16
#define FONT_TITLE_H 30

typedef enum {
    FONT_SMALL  = 0,
    FONT_NORMAL = 1,
    FONT_LARGE  = 2,
    FONT_BOLD   = 3,
    FONT_TITLE  = 4
} font_size_t;

void font_draw_char(int x, int y, char c, color_t fg, color_t bg, font_size_t size);
void font_draw_string(int x, int y, const char *str, color_t fg, color_t bg, font_size_t size);
void font_draw_string_shadow(int x, int y, const char *str, color_t fg, font_size_t size);
int  font_string_width(const char *str, font_size_t size);
int  font_char_width(font_size_t size);
int  font_char_height(font_size_t size);

extern const uint8_t font8x16[256][16];

#endif
