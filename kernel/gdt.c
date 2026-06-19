
#include "gdt.h"
#include <stdint.h>

#define GDT_ENTRIES 5

static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdt_ptr;

static void set_gate(int i, uint8_t access, uint8_t gran) {
    gdt[i].limit_low = 0xFFFF;
    gdt[i].base_low  = 0;
    gdt[i].base_mid  = 0;
    gdt[i].access    = access;
    gdt[i].gran      = gran;
    gdt[i].base_high = 0;
}

extern void gdt64_flush(uint64_t ptr, uint16_t cs, uint16_t ds);

void gdt_init(void) {
    gdt_ptr.limit = sizeof(gdt_entry_t) * GDT_ENTRIES - 1;
    gdt_ptr.base  = (uint64_t)&gdt[0];

    gdt[0] = (gdt_entry_t){0};

    set_gate(1, 0x9A, 0xAF);

    set_gate(2, 0x92, 0xCF);

    set_gate(3, 0xFA, 0xAF);

    set_gate(4, 0xF2, 0xCF);

    gdt64_flush((uint64_t)&gdt_ptr, 0x08, 0x10);
}
