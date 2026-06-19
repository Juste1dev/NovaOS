
#include "vbe.h"
#include "../kernel/memory.h"
#include <stdint.h>
#include <stddef.h>

vbe_info_t vbe;

static inline uint32_t bpp_bytes(void) { return (vbe.bpp + 7) / 8; }

static inline void write_px(uint8_t *dst, color_t c) {
    dst[0] = (uint8_t)(c & 0xFF);
    dst[1] = (uint8_t)((c >> 8)  & 0xFF);
    dst[2] = (uint8_t)((c >> 16) & 0xFF);
    if (bpp_bytes() == 4) dst[3] = 0;
}

static inline color_t read_px(const uint8_t *src) {
    return RGB(src[2], src[1], src[0]);
}

void vbe_init(uint64_t fb_addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp) {
    vbe.fb_phys     = fb_addr;
    vbe.framebuffer = (uint8_t*)(uintptr_t)fb_addr;
    vbe.width       = w;
    vbe.height      = h;
    vbe.bpp         = bpp ? bpp : 32;
    vbe.pitch       = pitch ? pitch : (w * bpp_bytes());

    vbe.backbuffer = (uint8_t*)kmalloc((size_t)vbe.pitch * vbe.height);
    if (!vbe.backbuffer) vbe.backbuffer = vbe.framebuffer;
    else __builtin_memset(vbe.backbuffer, 0, (size_t)vbe.pitch * vbe.height);
}

void vbe_swap(void) {
    if (!vbe.backbuffer || vbe.backbuffer == vbe.framebuffer) return;
    uint32_t lb = vbe.width * bpp_bytes();
    for (uint32_t y = 0; y < vbe.height; y++)
        __builtin_memcpy(vbe.framebuffer + y*vbe.pitch,
                         vbe.backbuffer  + y*vbe.pitch, lb);
}

void vbe_swap_rect(int x, int y, int w, int h) {
    if (!vbe.backbuffer || vbe.backbuffer == vbe.framebuffer) return;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w;
    int y2 = y + h;
    if (x2 > (int)vbe.width) x2 = (int)vbe.width;
    if (y2 > (int)vbe.height) y2 = (int)vbe.height;
    if (x1 >= x2 || y1 >= y2) return;

    uint32_t bp = bpp_bytes();
    uint32_t row_bytes = (uint32_t)(x2 - x1) * bp;
    for (int py = y1; py < y2; py++) {
        __builtin_memcpy(vbe.framebuffer + py * vbe.pitch + x1 * bp,
                         vbe.backbuffer + py * vbe.pitch + x1 * bp,
                         row_bytes);
    }
}

color_t blend_color(color_t fg, color_t bg, uint8_t a) {
    uint8_t ia = 255 - a;
    uint8_t r = (uint8_t)(((uint32_t)COLOR_R(fg)*a + (uint32_t)COLOR_R(bg)*ia) >> 8);
    uint8_t g = (uint8_t)(((uint32_t)COLOR_G(fg)*a + (uint32_t)COLOR_G(bg)*ia) >> 8);
    uint8_t b = (uint8_t)(((uint32_t)COLOR_B(fg)*a + (uint32_t)COLOR_B(bg)*ia) >> 8);
    return RGB(r,g,b);
}

static void put_raw(int x, int y, color_t c) {
    if ((unsigned)x >= vbe.width || (unsigned)y >= vbe.height) return;
    write_px(vbe.backbuffer + y*vbe.pitch + x*bpp_bytes(), c);
}

static color_t get_raw(int x, int y) {
    if ((unsigned)x >= vbe.width || (unsigned)y >= vbe.height) return 0;
    return read_px(vbe.backbuffer + y*vbe.pitch + x*bpp_bytes());
}

void vbe_put_pixel(int x, int y, color_t c) { put_raw(x,y,c); }
color_t vbe_get_pixel(int x, int y) { return get_raw(x,y); }

void vbe_clear(color_t c) {
    uint32_t bp = bpp_bytes();
    for (uint32_t y = 0; y < vbe.height; y++) {
        uint8_t *row = vbe.backbuffer + y*vbe.pitch;
        for (uint32_t x = 0; x < vbe.width; x++) write_px(row + x*bp, c);
    }
}

void vbe_rect(int x, int y, int w, int h, color_t c) {
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x+w > (int)vbe.width  ? (int)vbe.width  : x+w;
    int y2 = y+h > (int)vbe.height ? (int)vbe.height : y+h;
    uint32_t bp = bpp_bytes();
    for (int py = y1; py < y2; py++) {
        uint8_t *row = vbe.backbuffer + py*vbe.pitch;
        for (int px = x1; px < x2; px++) write_px(row + px*bp, c);
    }
}

void vbe_rect_outline(int x,int y,int w,int h,int t,color_t c) {
    vbe_rect(x,y,w,t,c);
    vbe_rect(x,y+h-t,w,t,c);
    vbe_rect(x,y,t,h,c);
    vbe_rect(x+w-t,y,t,h,c);
}

void vbe_line(int x1,int y1,int x2,int y2,color_t c) {
    int dx = x2>x1?x2-x1:x1-x2, dy = y2>y1?y2-y1:y1-y2;
    int sx = x1<x2?1:-1, sy = y1<y2?1:-1;
    int err = (dx>dy?dx:-dy)/2;
    for(;;) {
        put_raw(x1,y1,c);
        if (x1==x2 && y1==y2) break;
        int e2=err;
        if (e2>-dx){err-=dy;x1+=sx;}
        if (e2< dy){err+=dx;y1+=sy;}
    }
}

void vbe_line_thick(int x1, int y1, int x2, int y2, int t, color_t c) {
    for (int i = -t/2; i <= t/2; i++) {
        int dx = x2-x1, dy = y2-y1;
        int len = dx*dx + dy*dy;
        if (len == 0) { vbe_rect(x1-t/2, y1-t/2, t, t, c); return; }

        vbe_line(x1, y1+i, x2, y2+i, c);
        vbe_line(x1+i, y1, x2+i, y2, c);
    }
}

void vbe_circle(int cx, int cy, int r, color_t c) {
    int x=r,y=0,err=0;
    while(x>=y){
        put_raw(cx+x,cy+y,c); put_raw(cx+y,cy+x,c);
        put_raw(cx-y,cy+x,c); put_raw(cx-x,cy+y,c);
        put_raw(cx-x,cy-y,c); put_raw(cx-y,cy-x,c);
        put_raw(cx+y,cy-x,c); put_raw(cx+x,cy-y,c);
        if(err<=0){y++;err+=2*y+1;}
        if(err>0){x--;err-=2*x+1;}
    }
}

void vbe_circle_fill(int cx, int cy, int r, color_t c) {
    int x=r,y=0,err=0;
    while(x>=y){
        vbe_rect(cx-x,cy+y,2*x,1,c);
        vbe_rect(cx-y,cy+x,2*y,1,c);
        vbe_rect(cx-x,cy-y,2*x,1,c);
        vbe_rect(cx-y,cy-x,2*y,1,c);
        if(err<=0){y++;err+=2*y+1;}
        if(err>0){x--;err-=2*x+1;}
    }
}

void vbe_gradient_v(int x, int y, int w, int h, color_t top, color_t bot) {
    for (int py = 0; py < h; py++) {
        uint8_t t = (uint8_t)((py * 255) / (h > 1 ? h-1 : 1));
        uint8_t r = (uint8_t)((COLOR_R(top)*(255-t) + COLOR_R(bot)*t) >> 8);
        uint8_t g = (uint8_t)((COLOR_G(top)*(255-t) + COLOR_G(bot)*t) >> 8);
        uint8_t b = (uint8_t)((COLOR_B(top)*(255-t) + COLOR_B(bot)*t) >> 8);
        vbe_rect(x, y+py, w, 1, RGB(r,g,b));
    }
}

void vbe_gradient_h(int x, int y, int w, int h, color_t left, color_t right) {
    for (int px = 0; px < w; px++) {
        uint8_t t = (uint8_t)((px * 255) / (w > 1 ? w-1 : 1));
        uint8_t r = (uint8_t)((COLOR_R(left)*(255-t) + COLOR_R(right)*t) >> 8);
        uint8_t g = (uint8_t)((COLOR_G(left)*(255-t) + COLOR_G(right)*t) >> 8);
        uint8_t b = (uint8_t)((COLOR_B(left)*(255-t) + COLOR_B(right)*t) >> 8);
        vbe_rect(x+px, y, 1, h, RGB(r,g,b));
    }
}

void vbe_gradient_radial(int cx, int cy, int r, color_t inner, color_t outer) {
    for (int y = cy-r; y <= cy+r; y++) {
        for (int x = cx-r; x <= cx+r; x++) {
            int dx = x-cx, dy = y-cy;
            int d2 = dx*dx + dy*dy;
            if (d2 > r*r) continue;

            int d = 0;
            int tmp = d2;
            while ((d+1)*(d+1) <= tmp) d++;
            uint8_t t = (uint8_t)((d * 255) / (r > 0 ? r : 1));
            color_t c = blend_color(outer, inner, t);
            put_raw(x, y, c);
        }
    }
}

void vbe_rounded_rect(int x, int y, int w, int h, int r, color_t c) {
    if (r < 1) { vbe_rect(x,y,w,h,c); return; }
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;

    vbe_rect(x+r, y,   w-2*r, h,   c);
    vbe_rect(x,   y+r, r,     h-2*r, c);
    vbe_rect(x+w-r, y+r, r,   h-2*r, c);

    int cx1 = x+r, cy1 = y+r;
    int cx2 = x+w-r-1, cy2 = y+r;
    int cx3 = x+r, cy3 = y+h-r-1;
    int cx4 = x+w-r-1, cy4 = y+h-r-1;
    for (int py = 0; py <= r; py++) {
        for (int px = 0; px <= r; px++) {
            if (px*px + py*py <= r*r) {
                put_raw(cx1-px, cy1-py, c);
                put_raw(cx2+px, cy2-py, c);
                put_raw(cx3-px, cy3+py, c);
                put_raw(cx4+px, cy4+py, c);
            }
        }
    }
}

void vbe_rounded_rect_outline(int x, int y, int w, int h, int r, int t, color_t c) {
    for (int i = 0; i < t; i++)
        vbe_rounded_rect(x+i, y+i, w-2*i, h-2*i, r > i ? r-i : 0, c);
}

void vbe_blend_rect(int x, int y, int w, int h, color_t c, uint8_t alpha) {
    if (alpha == 0) return;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x+w > (int)vbe.width  ? (int)vbe.width  : x+w;
    int y2 = y+h > (int)vbe.height ? (int)vbe.height : y+h;
    for (int py = y1; py < y2; py++)
        for (int px = x1; px < x2; px++)
            put_raw(px, py, blend_color(c, get_raw(px,py), alpha));
}

void vbe_blend_rounded_rect(int x, int y, int w, int h, int r, color_t c, uint8_t alpha) {
    if (alpha == 0) return;
    if (r < 1) { vbe_blend_rect(x,y,w,h,c,alpha); return; }
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    for (int py = y; py < y+h; py++) {
        for (int px = x; px < x+w; px++) {
            int in = 1;

            if (px < x+r && py < y+r) {
                int dx = px-(x+r), dy = py-(y+r);
                in = (dx*dx + dy*dy <= r*r);
            } else if (px >= x+w-r && py < y+r) {
                int dx = px-(x+w-r), dy = py-(y+r);
                in = (dx*dx + dy*dy <= r*r);
            } else if (px < x+r && py >= y+h-r) {
                int dx = px-(x+r), dy = py-(y+h-r);
                in = (dx*dx + dy*dy <= r*r);
            } else if (px >= x+w-r && py >= y+h-r) {
                int dx = px-(x+w-r), dy = py-(y+h-r);
                in = (dx*dx + dy*dy <= r*r);
            }
            if (in && (unsigned)px < vbe.width && (unsigned)py < vbe.height)
                put_raw(px, py, blend_color(c, get_raw(px,py), alpha));
        }
    }
}

void vbe_glass_rect(int x, int y, int w, int h, int r, color_t tint, uint8_t alpha) {

    int x1 = x < 1 ? 1 : x;
    int y1 = y < 1 ? 1 : y;
    int x2 = x+w > (int)vbe.width-1  ? (int)vbe.width-1  : x+w;
    int y2 = y+h > (int)vbe.height-1 ? (int)vbe.height-1 : y+h;
    for (int py = y1; py < y2; py++) {
        for (int px = x1; px < x2; px++) {

            uint32_t rr=0, gg=0, bb=0;
            for (int dy=-1; dy<=1; dy++) for (int dx=-1; dx<=1; dx++) {
                color_t nb = get_raw(px+dx, py+dy);
                rr += COLOR_R(nb); gg += COLOR_G(nb); bb += COLOR_B(nb);
            }
            color_t blurred = RGB(rr/9, gg/9, bb/9);
            put_raw(px, py, blend_color(tint, blurred, alpha));
        }
    }

    vbe_rounded_rect_outline(x, y, w, h, r, 1, RGBA(255,255,255,60));
}

void vbe_blit(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh) {
    (void)sw; (void)sh;
    uint32_t bp = bpp_bytes();
    for (int py = 0; py < dh; py++) {
        for (int px = 0; px < dw; px++) {
            int rx = dx+px, ry = dy+py;
            if ((unsigned)rx >= vbe.width || (unsigned)ry >= vbe.height) continue;
            color_t c = src[py*dw + px];
            write_px(vbe.backbuffer + ry*vbe.pitch + rx*bp, c);
        }
    }
}

void vbe_triangle_fill(int x1,int y1,int x2,int y2,int x3,int y3,color_t c) {

    if (y1 > y2) { int t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }
    if (y1 > y3) { int t; t=x1;x1=x3;x3=t; t=y1;y1=y3;y3=t; }
    if (y2 > y3) { int t; t=x2;x2=x3;x3=t; t=y2;y2=y3;y3=t; }
    for (int y = y1; y <= y3; y++) {
        int xa, xb;
        if (y <= y2 && y2 != y1) {
            xa = x1 + (x2-x1)*(y-y1)/(y2-y1 ? y2-y1 : 1);
        } else if (y3 != y2) {
            xa = x2 + (x3-x2)*(y-y2)/(y3-y2 ? y3-y2 : 1);
        } else xa = x2;
        if (y3 != y1) xb = x1 + (x3-x1)*(y-y1)/(y3-y1);
        else xb = x1;
        if (xa > xb) { int t=xa; xa=xb; xb=t; }
        vbe_rect(xa, y, xb-xa+1, 1, c);
    }
}

void vbe_copy_region(int sx, int sy, int dx, int dy, int w, int h) {
    uint32_t bp = bpp_bytes();
    for (int y = 0; y < h; y++) {
        __builtin_memcpy(
            vbe.backbuffer + (dy+y)*vbe.pitch + dx*bp,
            vbe.backbuffer + (sy+y)*vbe.pitch + sx*bp,
            w * bp);
    }
}
