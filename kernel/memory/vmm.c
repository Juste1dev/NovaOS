#include "../memory.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t mapped_bytes;
    uint64_t regions;
    uint64_t last_base;
    uint64_t last_size;
    uint64_t last_flags;
} vmm_state_t;

static vmm_state_t g_vmm;

static int vmm_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void vmm_cat(char *dst, const char *src, int max) {
    int dl = vmm_len(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void vmm_u64(uint64_t value, char *buf, int max) {
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

static void vmm_hex(uint64_t value, char *buf, int max) {
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

void vmm_init(void) {
    g_vmm.mapped_bytes = 0;
    g_vmm.regions = 0;
    g_vmm.last_base = 0;
    g_vmm.last_size = 0;
    g_vmm.last_flags = 0;
}

void vmm_map_region(uint64_t base, uint64_t size, uint64_t flags) {
    if (!size) return;
    paging_map_region(base, size, flags);
    g_vmm.mapped_bytes += size;
    g_vmm.regions++;
    g_vmm.last_base = base;
    g_vmm.last_size = size;
    g_vmm.last_flags = flags;
}

uint64_t vmm_mapped_bytes(void) {
    return g_vmm.mapped_bytes;
}

uint64_t vmm_region_count(void) {
    return g_vmm.regions;
}

void vmm_report(char *buf, int max) {
    char nb[48];
    char hx[32];
    if (!buf || max <= 0) return;
    buf[0] = 0;
    vmm_cat(buf, "VMM: bootstrap mapper\n", max);
    vmm_cat(buf, "MappedBytes=", max); vmm_u64(g_vmm.mapped_bytes, nb, sizeof(nb)); vmm_cat(buf, nb, max); vmm_cat(buf, "\n", max);
    vmm_cat(buf, "MappedRegions=", max); vmm_u64(g_vmm.regions, nb, sizeof(nb)); vmm_cat(buf, nb, max); vmm_cat(buf, "\n", max);
    vmm_cat(buf, "LastBase=", max); vmm_hex(g_vmm.last_base, hx, sizeof(hx)); vmm_cat(buf, hx, max); vmm_cat(buf, "\n", max);
    vmm_cat(buf, "LastSize=", max); vmm_u64(g_vmm.last_size, nb, sizeof(nb)); vmm_cat(buf, nb, max); vmm_cat(buf, "\n", max);
    vmm_cat(buf, "LastFlags=", max); vmm_hex(g_vmm.last_flags, hx, sizeof(hx)); vmm_cat(buf, hx, max); vmm_cat(buf, "\n", max);
}
