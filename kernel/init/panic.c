#include "panic.h"
#include "early_console.h"
#include "../../drivers/vbe.h"
#include "../../gui/font.h"
#include "../../libc.h"

static char g_last_panic_msg[160];
static char g_last_panic_detail[320];
static uint64_t g_last_panic_fault_addr = 0;

static void panic_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void panic_cat(char *dst, const char *src, int max) {
    int dl = 0;
    int i = 0;
    if (!dst || !src || max <= 0) return;
    while (dst[dl] && dl < max - 1) dl++;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void panic_hex(uint64_t value, char *buf, int max) {
    static const char hex[] = "0123456789ABCDEF";
    if (!buf || max < 3) return;
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16 && i + 2 < max - 1; ++i) {
        int shift = (15 - i) * 4;
        buf[i + 2] = hex[(value >> shift) & 0xFULL];
    }
    buf[(max > 18) ? 18 : (max - 1)] = 0;
}

static void panic_append_reg_line(char *out, int max, const char *name, uint64_t value) {
    char hex[32];
    panic_cat(out, name, max);
    panic_cat(out, "=", max);
    panic_hex(value, hex, sizeof(hex));
    panic_cat(out, hex, max);
}

static void panic_draw_line(int x, int y, const char *text, color_t color) {
    if (!text || !text[0]) return;
    font_draw_string(x, y, (char *)text, color, COLOR_TRANS, FONT_SMALL);
}

const char *kernel_last_panic_message(void) {
    return g_last_panic_msg;
}

const char *kernel_last_panic_detail(void) {
    return g_last_panic_detail;
}

uint64_t kernel_last_panic_fault_addr(void) {
    return g_last_panic_fault_addr;
}

void kernel_panic_with_context(const char *msg, const registers_t *regs, uint64_t fault_addr, const char *detail) {
    char line1[160];
    char line2[160];
    char line3[160];
    char line4[160];
    char line5[160];
    char line6[160];
    char fault_hex[32];

    k_memset(g_last_panic_msg, 0, sizeof(g_last_panic_msg));
    k_memset(g_last_panic_detail, 0, sizeof(g_last_panic_detail));
    panic_copy(g_last_panic_msg, msg ? msg : "Kernel panic", sizeof(g_last_panic_msg));
    panic_copy(g_last_panic_detail, detail ? detail : "", sizeof(g_last_panic_detail));
    g_last_panic_fault_addr = fault_addr;

    early_print("[PANIC] ");
    early_print(g_last_panic_msg);
    early_print("\n");
    if (g_last_panic_detail[0]) {
        early_print("[DETAIL] ");
        early_print(g_last_panic_detail);
        early_print("\n");
    }
    if (fault_addr) {
        early_print("[FAULT] ");
        early_print_hex(fault_addr);
        early_print("\n");
    }
    if (regs) {
        early_print("[REGS] RIP="); early_print_hex(regs->rip);
        early_print(" RSP="); early_print_hex(regs->rsp);
        early_print(" RFLAGS="); early_print_hex(regs->rflags);
        early_print("\n");
        early_print("[REGS] RAX="); early_print_hex(regs->rax);
        early_print(" RBX="); early_print_hex(regs->rbx);
        early_print(" RCX="); early_print_hex(regs->rcx);
        early_print(" RDX="); early_print_hex(regs->rdx);
        early_print("\n");
        early_print("[REGS] RSI="); early_print_hex(regs->rsi);
        early_print(" RDI="); early_print_hex(regs->rdi);
        early_print(" RBP="); early_print_hex(regs->rbp);
        early_print(" INT="); early_print_hex(regs->int_no);
        early_print(" ERR="); early_print_hex(regs->err_code);
        early_print("\n");
    }

    if (vbe.framebuffer) {
        k_memset(line1, 0, sizeof(line1));
        k_memset(line2, 0, sizeof(line2));
        k_memset(line3, 0, sizeof(line3));
        k_memset(line4, 0, sizeof(line4));
        k_memset(line5, 0, sizeof(line5));
        k_memset(line6, 0, sizeof(line6));

        vbe_clear(RGB(12, 18, 44));
        vbe_gradient_v(0, 0, vbe.width, vbe.height, RGB(18, 28, 68), RGB(8, 12, 28));
        vbe_blend_rounded_rect(64, 64, vbe.width - 128, vbe.height - 128, 24, RGB(28, 34, 56), 236);
        vbe_rounded_rect_outline(64, 64, vbe.width - 128, vbe.height - 128, 24, 2, RGB(160, 88, 88));
        font_draw_string_shadow(96, 92, "KERNEL PANIC", RGB(255, 230, 230), FONT_TITLE);
        font_draw_string(96, 138, g_last_panic_msg, RGB(255, 180, 180), COLOR_TRANS, FONT_LARGE);
        if (g_last_panic_detail[0]) {
            font_draw_string(96, 184, g_last_panic_detail, RGB(220, 226, 240), COLOR_TRANS, FONT_NORMAL);
        }
        panic_hex(fault_addr, fault_hex, sizeof(fault_hex));
        if (fault_addr) {
            font_draw_string(96, 220, "Fault address:", RGB(140, 190, 255), COLOR_TRANS, FONT_NORMAL);
            font_draw_string(252, 220, fault_hex, RGB(255, 255, 255), COLOR_TRANS, FONT_NORMAL);
        }
        if (regs) {
            panic_append_reg_line(line1, sizeof(line1), "RIP", regs->rip);
            panic_append_reg_line(line1, sizeof(line1), "  RSP", regs->rsp);
            panic_append_reg_line(line1, sizeof(line1), "  RFLAGS", regs->rflags);
            panic_append_reg_line(line2, sizeof(line2), "RAX", regs->rax);
            panic_append_reg_line(line2, sizeof(line2), "  RBX", regs->rbx);
            panic_append_reg_line(line2, sizeof(line2), "  RCX", regs->rcx);
            panic_append_reg_line(line3, sizeof(line3), "RDX", regs->rdx);
            panic_append_reg_line(line3, sizeof(line3), "  RSI", regs->rsi);
            panic_append_reg_line(line3, sizeof(line3), "  RDI", regs->rdi);
            panic_append_reg_line(line4, sizeof(line4), "RBP", regs->rbp);
            panic_append_reg_line(line4, sizeof(line4), "  R8", regs->r8);
            panic_append_reg_line(line4, sizeof(line4), "  R9", regs->r9);
            panic_append_reg_line(line5, sizeof(line5), "R10", regs->r10);
            panic_append_reg_line(line5, sizeof(line5), "  R11", regs->r11);
            panic_append_reg_line(line5, sizeof(line5), "  R12", regs->r12);
            panic_append_reg_line(line6, sizeof(line6), "INT", regs->int_no);
            panic_append_reg_line(line6, sizeof(line6), "  ERR", regs->err_code);
            panic_append_reg_line(line6, sizeof(line6), "  R13", regs->r13);
            panic_draw_line(96, 280, line1, RGB(224, 232, 245));
            panic_draw_line(96, 312, line2, RGB(224, 232, 245));
            panic_draw_line(96, 344, line3, RGB(224, 232, 245));
            panic_draw_line(96, 376, line4, RGB(224, 232, 245));
            panic_draw_line(96, 408, line5, RGB(224, 232, 245));
            panic_draw_line(96, 440, line6, RGB(224, 232, 245));
        }
        font_draw_string(96, vbe.height - 112, "Kernel halted to avoid uncontrolled reboot/triple fault cascade.", RGB(255, 214, 140), COLOR_TRANS, FONT_NORMAL);
        font_draw_string(96, vbe.height - 80, "Redemarre QEMU apres avoir consulte le log serie et /proc/heapinfo si disponible.", RGB(186, 198, 220), COLOR_TRANS, FONT_SMALL);
        vbe_swap();
    }

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void kernel_panic(const char *msg) {
    kernel_panic_with_context(msg, NULL, 0, NULL);
}
