
#ifndef VBE_H
#define VBE_H
#include <stdint.h>

#define SCREEN_WIDTH   1920
#define SCREEN_HEIGHT  1080
#define SCREEN_BPP     32

typedef uint32_t color_t;

#define RGB(r,g,b)    ((color_t)(((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b)))
#define RGBA(r,g,b,a) ((color_t)(((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b)))
#define COLOR_R(c)    (((c)>>16)&0xFF)
#define COLOR_G(c)    (((c)>>8)&0xFF)
#define COLOR_B(c)    ((c)&0xFF)
#define COLOR_A(c)    (((c)>>24)&0xFF)

#define COLOR_BLACK   RGB(0,0,0)
#define COLOR_WHITE   RGB(255,255,255)
#define COLOR_RED     RGB(220,50,50)
#define COLOR_GREEN   RGB(50,200,80)
#define COLOR_BLUE    RGB(50,120,230)
#define COLOR_YELLOW  RGB(255,228,0)
#define COLOR_CYAN    RGB(0,210,225)
#define COLOR_ORANGE  RGB(255,150,0)
#define COLOR_PURPLE  RGB(160,80,220)
#define COLOR_GRAY    RGB(128,128,128)
#define COLOR_DGRAY   RGB(60,60,70)
#define COLOR_LGRAY   RGB(200,205,215)
#define COLOR_TRANS   0xFF000000u

#define THEME_BG        RGB(6, 8, 18)
#define THEME_BG2       RGB(12, 16, 32)
#define THEME_BAR       RGB(10, 12, 22)
#define THEME_BAR2      RGB(8, 10, 18)
#define THEME_ACCENT    RGB(99, 179, 255)
#define THEME_ACCENT2   RGB(168, 100, 255)
#define THEME_ACCENT3   RGB(255, 100, 180)
#define THEME_ACCENT4   RGB(50, 220, 180)
#define THEME_WIN_BG    RGB(16, 20, 36)
#define THEME_WIN_BG2   RGB(20, 26, 44)
#define THEME_WIN_TIT   RGB(18, 24, 42)
#define THEME_WIN_TIT2  RGB(12, 16, 30)
#define THEME_DARK      RGB(14, 18, 32)
#define THEME_HOVER     RGB(40, 54, 90)
#define THEME_TEXT      RGB(220, 230, 255)
#define THEME_TEXT2     RGB(160, 180, 220)
#define THEME_TEXT_DRK  RGB(30, 40, 60)
#define THEME_BORDER    RGB(60, 100, 180)
#define THEME_BORDER2   RGB(80, 120, 200)
#define THEME_SHADOW    RGBA(0,0,8,130)
#define THEME_GLASS     RGBA(200,220,255,18)
#define THEME_GLASS2    RGBA(180,200,255,28)
#define THEME_SUCCESS   RGB(50, 200, 100)
#define THEME_WARNING   RGB(255, 180, 0)
#define THEME_ERROR     RGB(220, 60, 60)
#define THEME_INFO      RGB(60, 160, 255)

typedef struct {
    uint8_t  *framebuffer;
    uint8_t  *backbuffer;
    uint64_t  fb_phys;
    uint32_t  width, height, pitch, bpp;
} vbe_info_t;

extern vbe_info_t vbe;

void    vbe_init(uint64_t fb_addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp);
void    vbe_swap(void);
void    vbe_swap_rect(int x, int y, int w, int h);
void    vbe_clear(color_t color);
void    vbe_put_pixel(int x, int y, color_t c);
color_t vbe_get_pixel(int x, int y);
void    vbe_rect(int x, int y, int w, int h, color_t c);
void    vbe_rect_outline(int x, int y, int w, int h, int t, color_t c);
void    vbe_line(int x1, int y1, int x2, int y2, color_t c);
void    vbe_line_thick(int x1, int y1, int x2, int y2, int t, color_t c);
void    vbe_circle(int cx, int cy, int r, color_t c);
void    vbe_circle_fill(int cx, int cy, int r, color_t c);
void    vbe_gradient_v(int x, int y, int w, int h, color_t top, color_t bot);
void    vbe_gradient_h(int x, int y, int w, int h, color_t left, color_t right);
void    vbe_gradient_radial(int cx, int cy, int r, color_t inner, color_t outer);
void    vbe_rounded_rect(int x, int y, int w, int h, int r, color_t c);
void    vbe_rounded_rect_outline(int x, int y, int w, int h, int r, int t, color_t c);
void    vbe_blend_rect(int x, int y, int w, int h, color_t c, uint8_t alpha);
void    vbe_blend_rounded_rect(int x, int y, int w, int h, int r, color_t c, uint8_t alpha);
void    vbe_glass_rect(int x, int y, int w, int h, int r, color_t tint, uint8_t alpha);
void    vbe_blit(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh);
color_t blend_color(color_t fg, color_t bg, uint8_t alpha);
void    vbe_triangle_fill(int x1,int y1,int x2,int y2,int x3,int y3,color_t c);
void    vbe_copy_region(int sx, int sy, int dx, int dy, int w, int h);

#endif
