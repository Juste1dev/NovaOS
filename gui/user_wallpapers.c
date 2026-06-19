#include "user_wallpapers.h"

#define USER_WP_W 1920
#define USER_WP_H 1080

extern const unsigned char _binary_assets_wallpapers_wp00_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp01_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp02_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp03_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp04_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp05_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp06_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp07_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp08_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp09_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp10_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp11_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp12_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp13_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp14_bin_start[];
extern const unsigned char _binary_assets_wallpapers_wp15_bin_start[];

static const ui_bitmap_t user_wallpaper_00_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp00_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_01_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp01_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_02_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp02_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_03_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp03_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_04_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp04_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_05_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp05_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_06_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp06_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_07_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp07_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_08_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp08_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_09_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp09_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_10_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp10_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_11_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp11_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_12_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp12_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_13_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp13_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_14_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp14_bin_start, USER_WP_W, USER_WP_H };
static const ui_bitmap_t user_wallpaper_15_bmp = { (const uint32_t*)_binary_assets_wallpapers_wp15_bin_start, USER_WP_W, USER_WP_H };

static const ui_bitmap_t *g_user_wallpapers[] = {
    &user_wallpaper_00_bmp,
    &user_wallpaper_01_bmp,
    &user_wallpaper_02_bmp,
    &user_wallpaper_03_bmp,
    &user_wallpaper_04_bmp,
    &user_wallpaper_05_bmp,
    &user_wallpaper_06_bmp,
    &user_wallpaper_07_bmp,
    &user_wallpaper_08_bmp,
    &user_wallpaper_09_bmp,
    &user_wallpaper_10_bmp,
    &user_wallpaper_11_bmp,
    &user_wallpaper_12_bmp,
    &user_wallpaper_13_bmp,
    &user_wallpaper_14_bmp,
    &user_wallpaper_15_bmp,
};

int user_wallpaper_count(void) {
    return (int)(sizeof(g_user_wallpapers) / sizeof(g_user_wallpapers[0]));
}

const ui_bitmap_t *user_wallpaper_get(int index) {
    int count = user_wallpaper_count();
    if (count <= 0) return 0;
    if (index < 0) index = -index;
    return g_user_wallpapers[index % count];
}
