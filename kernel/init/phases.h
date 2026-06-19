#ifndef NOVA_BOOT_PHASES_H
#define NOVA_BOOT_PHASES_H

#include <stdint.h>
#include "boot_context.h"

void boot_initialize_platform(const multiboot_info_t *mbi,
                              uint64_t fb_addr,
                              uint32_t fb_w,
                              uint32_t fb_h,
                              uint32_t fb_pitch,
                              uint32_t fb_bpp);
void boot_show_splash(void);
void boot_start_gui(void);

#endif
