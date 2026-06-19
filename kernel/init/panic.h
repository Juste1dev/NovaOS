#ifndef NOVA_PANIC_H
#define NOVA_PANIC_H

#include "../idt.h"
#include <stdint.h>

void kernel_panic(const char *msg);
void kernel_panic_with_context(const char *msg, const registers_t *regs, uint64_t fault_addr, const char *detail);
const char *kernel_last_panic_message(void);
const char *kernel_last_panic_detail(void);
uint64_t kernel_last_panic_fault_addr(void);

#endif
