#include "boot_context.h"
#include "early_console.h"
#include "phases.h"
#include "panic.h"
#include "../../drivers/vbe.h"

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    uint64_t fb_addr = SCREEN_WIDTH * 4ULL;
    uint32_t fb_w = SCREEN_WIDTH;
    uint32_t fb_h = SCREEN_HEIGHT;
    uint32_t fb_pitch = SCREEN_WIDTH * 4U;
    uint32_t fb_bpp = 32;

    serial_init();
    early_clear();
    early_print("[NovaOS] Boot sequence start.\n");

    if (magic != MULTIBOOT_MAGIC) {
        early_print("[PANIC] Bad multiboot magic!\n");
        __asm__ volatile("cli; hlt");
        return;
    }

    if (mbi->flags & MB_FLAG_FB) {
        fb_addr = mbi->framebuffer_addr;
        fb_w = mbi->framebuffer_width;
        fb_h = mbi->framebuffer_height;
        fb_pitch = mbi->framebuffer_pitch;
        fb_bpp = mbi->framebuffer_bpp;
    }

    boot_initialize_platform(mbi, fb_addr, fb_w, fb_h, fb_pitch, fb_bpp);
    early_print("[Userspace] Ready\n");
    early_print("[NovaOS] Platform initialized.\n");
    boot_show_splash();
    early_print("[NovaOS] Entering GUI main loop...\n");
    boot_start_gui();
    kernel_panic("Boot sequence returned unexpectedly.");
}
