#ifndef NOVA_EARLY_CONSOLE_H
#define NOVA_EARLY_CONSOLE_H

#include <stdint.h>

void serial_init(void);
void early_clear(void);
void early_pc(char c);
void early_print(const char *s);
void early_print_dec(uint64_t value);
void early_print_hex(uint64_t value);

#endif
