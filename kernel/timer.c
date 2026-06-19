

#include "timer.h"
#include "idt.h"
#include <stddef.h>
#include <stdint.h>

static volatile uint32_t tick_count  = 0;
static volatile uint32_t timer_hz_val = 1000;
static timer_callback_t timer_cb = NULL;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void timer_irq_handler(registers_t *regs) {
    (void)regs;
    tick_count++;
    if (timer_cb) timer_cb();
}

void timer_init(uint32_t hz) {
    timer_hz_val = hz;
    uint32_t divisor = 1193180 / hz;
    register_interrupt_handler(32, timer_irq_handler);
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

uint32_t timer_ticks(void) { return tick_count; }
uint32_t timer_ms(void)    { return tick_count * 1000 / timer_hz_val; }

void timer_sleep(uint32_t ms) {
    uint32_t start = timer_ms();
    while (timer_ms() - start < ms)
        __asm__ volatile ("hlt");
}

void timer_register_callback(timer_callback_t cb) { timer_cb = cb; }
