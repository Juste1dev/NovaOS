#include "init/panic.h"
#include <stdint.h>

uintptr_t __stack_chk_guard = 0x9D4DF8A16C27B39FULL;

__attribute__((noreturn)) void __stack_chk_fail(void) {
    kernel_panic("Stack canary corruption detected.");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
