

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

typedef void (*timer_callback_t)(void);

void timer_init(uint32_t hz);
uint32_t timer_ticks(void);
uint32_t timer_ms(void);
void timer_sleep(uint32_t ms);
void timer_register_callback(timer_callback_t cb);

#endif
