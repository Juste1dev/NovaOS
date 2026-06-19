#include "../memory.h"
#include <stdint.h>

typedef struct {
    uint64_t mapped_bytes;
    uint64_t last_flags;
    int ready;
} paging_state_t;

static paging_state_t g_paging;

void paging_init(void) {
    g_paging.ready = 1;
    g_paging.mapped_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    g_paging.last_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
}

void paging_map_region(uint64_t base, uint64_t size, uint64_t flags) {
    (void)base;
    g_paging.mapped_bytes += size;
    g_paging.last_flags = flags;
}

uint64_t paging_flags_default(void) {
    return g_paging.last_flags ? g_paging.last_flags : (PAGE_PRESENT | PAGE_WRITABLE);
}
