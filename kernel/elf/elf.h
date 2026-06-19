#ifndef NOVA_ELF_H
#define NOVA_ELF_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int valid;
    int is_elf;
    int is_script;
    uint64_t entry_point;
    uint64_t user_stack_top;
    uint64_t image_size;
    uint32_t segment_count;
    char format[16];
    char arch[16];
    char interpreter[64];
    char path[64];
} nova_elf_image_t;

void elf_init(void);
int  elf_load_from_memory(const uint8_t *data, size_t len, const char *path, nova_elf_image_t *out);
int  elf_load_from_vfs(const char *path, nova_elf_image_t *out);
void elf_publish_demo(void);
void elf_runtime_report(char *buf, int max);

#endif
