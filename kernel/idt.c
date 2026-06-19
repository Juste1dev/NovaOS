
#include "idt.h"
#include "init/panic.h"
#include "init/early_console.h"
#include "../libc.h"
#include <stdint.h>

#define IDT_ENTRIES 256

static idt_entry_t idt_entries[IDT_ENTRIES];
static idt_ptr_t   idt_ptr;
static isr_t       interrupt_handlers[IDT_ENTRIES];

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void); extern void isr128(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);
extern void idt_flush(uint64_t ptr);

static const char *exception_names[32] = {
    "Divide-by-zero fault",
    "Debug exception",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "BOUND range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved exception",
    "x87 floating-point exception",
    "Alignment check",
    "Machine check",
    "SIMD floating-point exception",
    "Virtualization exception",
    "Control protection exception",
    "Reserved exception 22",
    "Reserved exception 23",
    "Reserved exception 24",
    "Reserved exception 25",
    "Reserved exception 26",
    "Reserved exception 27",
    "Hypervisor injection exception",
    "VMM communication exception",
    "Security exception",
    "Reserved exception 31"
};

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static uint64_t read_cr2(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void idt_cat(char *dst, const char *src, int max) {
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

static void idt_u64(uint64_t value, char *buf, int max) {
    char tmp[32];
    int pos = 0;
    int out = 0;
    if (!buf || max <= 0) return;
    if (!value) {
        buf[0] = '0';
        if (max > 1) buf[1] = 0;
        return;
    }
    while (value && pos < (int)sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}

static void idt_hex(uint64_t value, char *buf, int max) {
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

static const char *idt_table_name(uint64_t err_code) {
    switch ((err_code >> 1) & 0x3ULL) {
        case 0: return "GDT";
        case 1: return "IDT";
        case 2: return "LDT";
        default: return "IDT";
    }
}

static void idt_build_gpf_detail(const registers_t *r, char *buf, int max) {
    char nb[32];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    idt_cat(buf, "Protection violation", max);
    if (!r) return;
    if (r->err_code) {
        idt_cat(buf, " | table=", max);
        idt_cat(buf, idt_table_name(r->err_code), max);
        idt_cat(buf, " selector=", max);
        idt_u64((r->err_code >> 3) & 0x1FFFULL, nb, sizeof(nb));
        idt_cat(buf, nb, max);
        if (r->err_code & 0x1ULL) idt_cat(buf, " ext", max);
    }
    if (r->cs & 0x3ULL) idt_cat(buf, " | ring3", max);
    else idt_cat(buf, " | ring0", max);
}

static void idt_build_pf_detail(const registers_t *r, uint64_t fault_addr, char *buf, int max) {
    char hex[32];
    uint64_t err = r ? r->err_code : 0;
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    idt_cat(buf, "addr=", max);
    idt_hex(fault_addr, hex, sizeof(hex));
    idt_cat(buf, hex, max);
    idt_cat(buf, " | ", max);
    idt_cat(buf, (err & 0x1ULL) ? "protection" : "non-present", max);
    idt_cat(buf, " | ", max);
    idt_cat(buf, (err & 0x2ULL) ? "write" : "read", max);
    idt_cat(buf, " | ", max);
    idt_cat(buf, (err & 0x4ULL) ? "user" : "supervisor", max);
    if (err & 0x8ULL) idt_cat(buf, " | reserved-bit", max);
    if (err & 0x10ULL) idt_cat(buf, " | instruction-fetch", max);
    if (err & 0x20ULL) idt_cat(buf, " | protection-key", max);
    if (err & 0x40ULL) idt_cat(buf, " | shadow-stack", max);
}

static void cpu_exception_handler(registers_t *r) {
    char detail[256];
    uint64_t fault_addr = 0;
    const char *name = "CPU exception";
    if (r && r->int_no < 32) name = exception_names[r->int_no];
    k_memset(detail, 0, sizeof(detail));

    if (!r) {
        kernel_panic_with_context(name, NULL, 0, "No register frame available.");
        return;
    }

    if (r->int_no == 13) {
        idt_build_gpf_detail(r, detail, sizeof(detail));
    } else if (r->int_no == 14) {
        fault_addr = read_cr2();
        idt_build_pf_detail(r, fault_addr, detail, sizeof(detail));
    } else if (r->int_no == 8) {
        idt_cat(detail, "Double fault trapped before uncontrolled reboot.", sizeof(detail));
    } else {
        idt_cat(detail, "Unhandled CPU exception trapped by IDT safety layer.", sizeof(detail));
    }

    kernel_panic_with_context(name, r, fault_addr, detail);
}

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].base_lo  = (uint16_t)(base & 0xFFFF);
    idt_entries[num].sel      = sel;
    idt_entries[num].ist      = 0;
    idt_entries[num].flags    = flags;
    idt_entries[num].base_mid = (uint16_t)((base >> 16) & 0xFFFF);
    idt_entries[num].base_hi  = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt_entries[num].reserved = 0;
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

void isr_handler(registers_t *r) {
    if (interrupt_handlers[r->int_no]) interrupt_handlers[r->int_no](r);
    else {
        early_print("[IDT] Unhandled ISR ");
        early_print_dec(r->int_no);
        early_print("\n");
        cpu_exception_handler(r);
    }
}

void irq_handler(registers_t *r) {
    if (r->int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    if (interrupt_handlers[r->int_no]) interrupt_handlers[r->int_no](r);
}

#define GATE(n, fn) idt_set_gate(n, (uint64_t)(fn), 0x08, 0x8E)
#define GATE_IRQ(n, fn) idt_set_gate(n, (uint64_t)(fn), 0x08, 0x8E)

void idt_init(void) {
    idt_ptr.limit = sizeof(idt_entry_t) * IDT_ENTRIES - 1;
    idt_ptr.base  = (uint64_t)&idt_entries[0];

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_entries[i] = (idt_entry_t){0};
        interrupt_handlers[i] = 0;
    }

    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0x00); outb(0xA1, 0x00);

    GATE(0,  isr0);  GATE(1,  isr1);  GATE(2,  isr2);  GATE(3,  isr3);
    GATE(4,  isr4);  GATE(5,  isr5);  GATE(6,  isr6);  GATE(7,  isr7);
    GATE(8,  isr8);  GATE(9,  isr9);  GATE(10, isr10); GATE(11, isr11);
    GATE(12, isr12); GATE(13, isr13); GATE(14, isr14); GATE(15, isr15);
    GATE(16, isr16); GATE(17, isr17); GATE(18, isr18); GATE(19, isr19);
    GATE(20, isr20); GATE(21, isr21); GATE(22, isr22); GATE(23, isr23);
    GATE(24, isr24); GATE(25, isr25); GATE(26, isr26); GATE(27, isr27);
    GATE(28, isr28); GATE(29, isr29); GATE(30, isr30); GATE(31, isr31);

    GATE_IRQ(32, irq0);  GATE_IRQ(33, irq1);  GATE_IRQ(34, irq2);
    GATE_IRQ(35, irq3);  GATE_IRQ(36, irq4);  GATE_IRQ(37, irq5);
    GATE_IRQ(38, irq6);  GATE_IRQ(39, irq7);  GATE_IRQ(40, irq8);
    GATE_IRQ(41, irq9);  GATE_IRQ(42, irq10); GATE_IRQ(43, irq11);
    GATE_IRQ(44, irq12); GATE_IRQ(45, irq13); GATE_IRQ(46, irq14);
    GATE_IRQ(47, irq15);

    for (int i = 0; i < 32; ++i) register_interrupt_handler((uint8_t)i, cpu_exception_handler);

    idt_set_gate(128, (uint64_t)isr128, 0x08, 0xEE);

    idt_flush((uint64_t)&idt_ptr);
}
