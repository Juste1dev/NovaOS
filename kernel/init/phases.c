#include "phases.h"
#include "early_console.h"
#include "panic.h"
#include "../../kernel/gdt.h"
#include "../../kernel/idt.h"
#include "../../kernel/memory.h"
#include "../../kernel/timer.h"
#include "../../kernel/userspace.h"
#include "../../kernel/platform_features.h"
#include "../../kernel/syscalls.h"
#include "../../kernel/ipc/ipc.h"
#include "../../kernel/elf/elf.h"
#include "../../kernel/sched/scheduler.h"
#include "../../kernel/pthread/pthread_runtime.h"
#include "../../kernel/module_loader.h"
#include "../../kernel/ssh.h"
#include "../../drivers/driver_manager.h"
#include "../../drivers/vbe.h"
#include "../../drivers/keyboard.h"
#include "../../drivers/mouse.h"
#include "../../drivers/sound.h"
#include "../../gui/gui.h"
#include "../../gui/font.h"
#include "../../gui/wayland.h"
#include "../../fs/vfs.h"
#include "../../net/net.h"
#include "../../apps/nova_pkg.h"
#include "../../libc.h"
#include <stdint.h>
#include <stddef.h>

static uint64_t boot_align_up(uint64_t value) {
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static int boot_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void boot_cat(char *dst, const char *src, int max) {
    int dl = boot_len(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void boot_init_pmm(const multiboot_info_t *mbi) {
    uint64_t mem_upper = 64ULL * 1024ULL;
    int used_mmap = 0;

    if (mbi->flags & MB_FLAG_MEM) mem_upper = mbi->mem_upper;
    pmm_init(mem_upper);

    if ((mbi->flags & MB_FLAG_MMAP) && mbi->mmap_addr && mbi->mmap_length) {
        uint32_t offset = 0;
        while (offset < mbi->mmap_length) {
            multiboot_mmap_entry_t *entry = (multiboot_mmap_entry_t *)(uintptr_t)((uint64_t)mbi->mmap_addr + offset);
            if (entry->type == MULTIBOOT_MMAP_AVAILABLE && entry->len) {
                pmm_init_region(entry->addr, entry->len);
                used_mmap = 1;
            }
            offset += entry->size + sizeof(entry->size);
        }
    }

    if (!used_mmap) {
        uint64_t total_mem = (mem_upper + 1024ULL) * 1024ULL;
        if (total_mem > 0x100000ULL) pmm_init_region(0x100000ULL, total_mem - 0x100000ULL);
    }

    pmm_reserve_region(0, boot_align_up((uint64_t)&_kernel_end));
    pmm_reserve_region(NOVA_HEAP_START, NOVA_HEAP_SIZE);
}

static void boot_publish_diagnostics(void) {
    char pmm[1024];
    char vmm[512];
    char heap[1536];
    char bootinfo[3072];

    if (!vfs_exists("/proc")) return;

    k_memset(pmm, 0, sizeof(pmm));
    k_memset(vmm, 0, sizeof(vmm));
    k_memset(heap, 0, sizeof(heap));
    k_memset(bootinfo, 0, sizeof(bootinfo));

    pmm_report(pmm, sizeof(pmm));
    vmm_report(vmm, sizeof(vmm));
    heap_debug_report(heap, sizeof(heap));

    boot_cat(bootinfo, "NovaOS Boot Diagnostics\n", sizeof(bootinfo));
    boot_cat(bootinfo, "=======================\n", sizeof(bootinfo));
    boot_cat(bootinfo, pmm, sizeof(bootinfo));
    boot_cat(bootinfo, "\n", sizeof(bootinfo));
    boot_cat(bootinfo, vmm, sizeof(bootinfo));
    boot_cat(bootinfo, "\n", sizeof(bootinfo));
    boot_cat(bootinfo, heap, sizeof(bootinfo));

    (void)vfs_write_file("/proc/pmm", pmm, (uint32_t)boot_len(pmm));
    (void)vfs_write_file("/proc/vmm", vmm, (uint32_t)boot_len(vmm));
    (void)vfs_write_file("/proc/bootinfo", bootinfo, (uint32_t)boot_len(bootinfo));
    (void)vfs_write_file("/var/log/boot.memory.log", bootinfo, (uint32_t)boot_len(bootinfo));
    (void)vfs_write_file("/home/user/Documents/Kernel-Memory.txt", bootinfo, (uint32_t)boot_len(bootinfo));
}

static void boot_log_persistence(void) {
    char persistence[192];
    char boot_count[32];
    vfs_get_contents("/proc/persistence", persistence, sizeof(persistence) - 1);
    vfs_get_contents("/var/log/boot.count", boot_count, sizeof(boot_count) - 1);
    (void)persistence;
    (void)boot_count;
}

static void boot_run_store_self_test(void) {
    char store_report[256];
    int store_ok;
    store_ok = nova_store_self_test(store_report, sizeof(store_report));
    if (store_report[0]) {
        early_print("[Applications] ");
        early_print(store_report);
        if (store_report[boot_len(store_report) - 1] != '\n') early_pc('\n');
    }
    vfs_write_file("/var/log/store-selftest.log", store_report, (uint32_t)boot_len(store_report));
    if (!store_ok) early_print("[Applications] Warning: self-test incomplete, system continues.\n");
}

void boot_initialize_platform(const multiboot_info_t *mbi,
                              uint64_t fb_addr,
                              uint32_t fb_w,
                              uint32_t fb_h,
                              uint32_t fb_pitch,
                              uint32_t fb_bpp) {
    uint64_t kernel_span = boot_align_up((uint64_t)&_kernel_end);
    uint64_t framebuffer_bytes = boot_align_up((uint64_t)fb_pitch * (uint64_t)fb_h);
    gdt_init();
    idt_init();
    boot_init_pmm(mbi);
    if (!(mbi->flags & MB_FLAG_FB) || !fb_addr || !fb_w || !fb_h) kernel_panic("Framebuffer VBE invalide ou absent.");
    pmm_reserve_region(fb_addr, framebuffer_bytes);
    paging_init();
    vmm_init();
    vmm_map_region(0, kernel_span, PAGE_PRESENT | PAGE_WRITABLE);
    vmm_map_region(NOVA_HEAP_START, NOVA_HEAP_SIZE, PAGE_PRESENT | PAGE_WRITABLE);
    vmm_map_region(fb_addr, framebuffer_bytes, PAGE_PRESENT | PAGE_WRITABLE);
    heap_init();
    timer_init(1000);
    scheduler_init();
    scheduler_install_timer_hook();
    syscall_init();
    keyboard_init();
    vbe_init(fb_addr, fb_w, fb_h, fb_pitch, fb_bpp);
    mouse_init();
    vfs_init();
    boot_publish_diagnostics();
    ipc_init();
    elf_init();
    userspace_init();
    userspace_publish_vfs();
    boot_log_persistence();
    net_init();
    ssh_init();
    pthread_runtime_init();
    wayland_init();
    nova_platform_features_init();

    boot_run_store_self_test();
    sound_init();
    driver_manager_init();
    module_loader_init();

    __asm__ volatile("sti");
    early_print("[NovaOS] Interrupts enabled.\n");
    sound_play_boot_jingle();
}

void boot_show_splash(void) {
    static const char *msgs[] = {
        NULL
    };
    int sw = (int)vbe.width;
    int sh = (int)vbe.height;
    int lx = sw / 2 - 200;
    int ly = sh / 2 - 110;

    vbe_clear(RGB(244, 248, 255));
    vbe_gradient_v(0, 0, sw, sh, RGB(246, 249, 255), RGB(228, 238, 252));
    vbe_blend_rounded_rect(lx + 6, ly + 10, 400, 220, 24, RGB(96, 118, 160), 22);
    vbe_blend_rounded_rect(lx, ly, 400, 220, 24, RGB(255, 255, 255), 236);
    vbe_rounded_rect_outline(lx, ly, 400, 220, 24, 1, RGB(196, 210, 236));
    vbe_gradient_v(lx, ly, 400, 52, RGB(245, 249, 255), RGB(232, 240, 255));
    vbe_gradient_h(lx + 20, ly + 18, 96, 12, RGB(104, 132, 255), RGB(110, 210, 255));

    for (int p = 0; p <= 100; ++p) {
        char pb[8];
        vbe_rect(lx + 20, ly + 120, 360, 20, RGB(228, 234, 244));
        { int bw = (360 * p) / 100; if (bw > 0) vbe_gradient_h(lx + 20, ly + 120, bw, 20, RGB(98, 142, 255), RGB(116, 210, 255)); }
        vbe_rounded_rect_outline(lx + 20, ly + 120, 360, 20, 6, 1, RGB(188, 198, 216));
        if (p < 10) { pb[0] = ' '; pb[1] = '0' + p; pb[2] = '%'; pb[3] = 0; }
        else if (p < 100) { pb[0] = '0' + (p / 10); pb[1] = '0' + (p % 10); pb[2] = '%'; pb[3] = 0; }
        else { pb[0] = '1'; pb[1] = '0'; pb[2] = '0'; pb[3] = '%'; pb[4] = 0; }
        font_draw_string(lx + 175, ly + 123, pb, COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
        vbe_swap();
        if (p < 40) timer_sleep(8); else if (p < 80) timer_sleep(5); else timer_sleep(15);
    }

    for (int i = 0; msgs[i]; ++i) {
        vbe_clear(RGB(244, 248, 255));
        vbe_gradient_v(0, 0, sw, sh, RGB(246, 249, 255), RGB(228, 238, 252));
        vbe_blend_rounded_rect(lx + 6, ly + 10, 400, 220, 24, RGB(96, 118, 160), 22);
        vbe_blend_rounded_rect(lx, ly, 400, 220, 24, RGB(255, 255, 255), 236);
        vbe_rounded_rect_outline(lx, ly, 400, 220, 24, 1, RGB(196, 210, 236));
        vbe_gradient_v(lx, ly, 400, 52, RGB(245, 249, 255), RGB(232, 240, 255));
        vbe_gradient_h(lx + 20, ly + 18, 96, 12, RGB(104, 132, 255), RGB(110, 210, 255));
            vbe_rect(lx + 20, ly + 120, 360, 20, RGB(228, 234, 244));
        vbe_gradient_h(lx + 20, ly + 120, 360, 20, RGB(98, 142, 255), RGB(116, 210, 255));
        font_draw_string(lx + 166, ly + 123, "100%", COLOR_WHITE, COLOR_TRANS, FONT_SMALL);
        vbe_swap();
        timer_sleep(250);
    }
}

void boot_start_gui(void) {
    gui_init();
    gui_main_loop();
    kernel_panic("GUI main loop exited unexpectedly!");
}
