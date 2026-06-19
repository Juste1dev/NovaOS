#include "early_console.h"
#include <stdint.h>

#define COM1 0x3F8

static volatile uint16_t *g_vga_mem = (volatile uint16_t*)0xB8000;
static int g_vga_x = 0;
static int g_vga_y = 0;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int serial_ready(void) {
    return inb(COM1 + 5) & 0x20;
}

static void serial_putc(char c) {
    while (!serial_ready()) {}
    outb(COM1, (uint8_t)c);
}

void early_pc(char c) {
    if (c == '\n') {
        g_vga_x = 0;
        g_vga_y++;
        serial_putc('\r');
        serial_putc('\n');
        return;
    }
    if (g_vga_y >= 25) {
        g_vga_x = 0;
        g_vga_y = 0;
    }
    g_vga_mem[g_vga_y * 80 + g_vga_x] = (uint16_t)(0x0F00u | (uint8_t)c);
    if (++g_vga_x >= 80) {
        g_vga_x = 0;
        g_vga_y++;
    }
    serial_putc(c);
}

void early_print(const char *s) {
    while (s && *s) early_pc(*s++);
}

void early_print_dec(uint64_t value) {
    char buf[24];
    int i = 0;
    if (!value) {
        early_pc('0');
        return;
    }
    while (value && i < (int)sizeof(buf) - 1) {
        buf[i++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (i--) early_pc(buf[i]);
}

void early_print_hex(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    early_print("0x");
    for (int i = 15; i >= 0; --i) {
        early_pc(hex[(value >> (i * 4)) & 0xFULL]);
    }
}

void early_clear(void) {
    for (int i = 0; i < 80 * 25; ++i) g_vga_mem[i] = 0x0720u;
    g_vga_x = 0;
    g_vga_y = 0;
}
