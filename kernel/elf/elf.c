#include "elf.h"
#include "../../fs/vfs.h"
#include "../../libc.h"
#include <stdint.h>
#include <stddef.h>

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

static int elf_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void elf_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void elf_cat(char *dst, const char *src, int max) {
    int dl = elf_len(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void elf_u64(uint64_t value, char *buf, int max) {
    char tmp[24];
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

static void elf_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void elf_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void elf_wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (i * 8)) & 0xFFu);
}

void elf_init(void) {
}

int elf_load_from_memory(const uint8_t *data, size_t len, const char *path, nova_elf_image_t *out) {
    if (!out) return 0;
    k_memset(out, 0, sizeof(*out));
    if (path) elf_copy(out->path, path, sizeof(out->path));
    out->user_stack_top = 0x00007FFF00000000ULL;
    out->image_size = len;
    if (!data || len < 2) return 0;

    if (len >= 2 && data[0] == '#' && data[1] == '!') {
        int i = 2;
        int pos = 0;
        out->valid = 1;
        out->is_script = 1;
        out->entry_point = 0x0000000000400000ULL;
        elf_copy(out->format, "script", sizeof(out->format));
        elf_copy(out->arch, "native", sizeof(out->arch));
        while ((size_t)i < len && data[i] != '\n' && pos < (int)sizeof(out->interpreter) - 1) {
            out->interpreter[pos++] = (char)data[i++];
        }
        out->interpreter[pos] = 0;
        out->segment_count = 1;
        return 1;
    }

    if (len < sizeof(elf64_ehdr_t)) return 0;
    {
        const elf64_ehdr_t *eh = (const elf64_ehdr_t *)data;
        if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) return 0;
        if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1 || eh->e_machine != 0x3E) return 0;
        if (eh->e_phoff + ((uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize) > len) return 0;
        out->valid = 1;
        out->is_elf = 1;
        out->entry_point = eh->e_entry;
        elf_copy(out->format, "ELF64", sizeof(out->format));
        elf_copy(out->arch, "x86_64", sizeof(out->arch));
        elf_copy(out->interpreter, "static", sizeof(out->interpreter));
        for (uint16_t i = 0; i < eh->e_phnum; ++i) {
            const elf64_phdr_t *ph = (const elf64_phdr_t *)(data + eh->e_phoff + ((uint64_t)i * eh->e_phentsize));
            if (ph->p_type == 1) out->segment_count++;
        }
        return 1;
    }
}

int elf_load_from_vfs(const char *path, nova_elf_image_t *out) {
    vfs_node_t *node = vfs_get_node(path);
    if (!node || !node->valid || node->type == VFS_TYPE_DIR) return 0;
    return elf_load_from_memory(node->data, node->size, path, out);
}

void elf_publish_demo(void) {
    uint8_t demo[120];
    k_memset(demo, 0, sizeof(demo));
    demo[0] = 0x7F; demo[1] = 'E'; demo[2] = 'L'; demo[3] = 'F';
    demo[4] = 2; demo[5] = 1; demo[6] = 1;
    elf_wr16(&demo[16], 2);
    elf_wr16(&demo[18], 0x3E);
    elf_wr32(&demo[20], 1);
    elf_wr64(&demo[24], 0x0000000000401000ULL);
    elf_wr64(&demo[32], 64);
    elf_wr64(&demo[40], 0);
    elf_wr32(&demo[48], 0);
    elf_wr16(&demo[52], 64);
    elf_wr16(&demo[54], 56);
    elf_wr16(&demo[56], 1);
    elf_wr16(&demo[58], 0);
    elf_wr16(&demo[60], 0);
    elf_wr16(&demo[62], 0);
    elf_wr32(&demo[64], 1);
    elf_wr32(&demo[68], 5);
    elf_wr64(&demo[72], 0);
    elf_wr64(&demo[80], 0x0000000000400000ULL);
    elf_wr64(&demo[88], 0x0000000000400000ULL);
    elf_wr64(&demo[96], sizeof(demo));
    elf_wr64(&demo[104], sizeof(demo));
    elf_wr64(&demo[112], 0x1000ULL);
    (void)vfs_mkdir("/usr");
    (void)vfs_mkdir("/usr/bin");
    (void)vfs_write_file("/usr/bin/demo.elf", (const char *)demo, sizeof(demo));
}

void elf_runtime_report(char *buf, int max) {
    static const char *paths[] = {
        "/usr/bin/initd",
        "/usr/bin/sessiond",
        "/usr/bin/ipcd",
        "/usr/bin/shmd",
        "/usr/bin/shell",
        "/usr/bin/nova-shell",
        "/usr/bin/browserd",
        "/usr/bin/demo.elf",
        NULL
    };
    char nb[24];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    elf_cat(buf, "Nova ELF loader\n", max);
    elf_cat(buf, "PATH TYPE ENTRY SEGMENTS INTERP\n", max);
    elf_cat(buf, "--------------------------------\n", max);
    for (int i = 0; paths[i]; ++i) {
        nova_elf_image_t img;
        if (!vfs_exists(paths[i])) continue;
        if (!elf_load_from_vfs(paths[i], &img)) continue;
        elf_cat(buf, paths[i], max); elf_cat(buf, " ", max);
        elf_cat(buf, img.format, max); elf_cat(buf, " ", max);
        elf_u64(img.entry_point, nb, sizeof(nb)); elf_cat(buf, nb, max); elf_cat(buf, " ", max);
        elf_u64(img.segment_count, nb, sizeof(nb)); elf_cat(buf, nb, max); elf_cat(buf, " ", max);
        elf_cat(buf, img.interpreter[0] ? img.interpreter : "-", max); elf_cat(buf, "\n", max);
    }
}
