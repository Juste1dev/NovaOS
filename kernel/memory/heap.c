#include "../memory.h"
#include "../../libc.h"
#include <stdint.h>
#include <stddef.h>

#define HEAP_START        NOVA_HEAP_START
#define HEAP_SIZE         NOVA_HEAP_SIZE
#define HEAP_MAGIC        0xCAFEBABEUL
#define HEAP_FREE_MAGIC   0xDEADF00DUL
#define HEAP_FRONT_GUARD  0xC0DEFACECAFED00DULL
#define HEAP_TAIL_GUARD   0xBADC0FFEE0DDF00DULL
#define HEAP_POISON_FREE  0xDD
#define HEAP_POISON_ALLOC 0xA5

#define BLOCK_FTR ((uint64_t)sizeof(uint64_t))

typedef struct heap_block {
    uint32_t magic;
    uint32_t flags;
    uint64_t size;
    uint64_t requested;
    uint64_t front_guard;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

#define BLOCK_HDR ((uint64_t)sizeof(heap_block_t))
#define HEAP_BLOCK_USED 0x1u

static uint64_t g_placement = 0;
static heap_block_t *g_heap_head = NULL;
static uint64_t g_heap_alloc_count = 0;
static uint64_t g_heap_free_count = 0;
static uint64_t g_heap_failed_allocs = 0;
static uint64_t g_heap_active_allocs = 0;
static uint64_t g_heap_peak_used = 0;
static uint64_t g_heap_corruption_events = 0;
static uint64_t g_heap_last_error_addr = 0;

static int heap_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void heap_cat(char *dst, const char *src, int max) {
    int dl = heap_len(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void heap_u64(uint64_t value, char *buf, int max) {
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

static void heap_hex(uint64_t value, char *buf, int max) {
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

static uint8_t *heap_user_ptr(heap_block_t *block) {
    return (uint8_t *)block + BLOCK_HDR;
}

static uint64_t *heap_tail_ptr(heap_block_t *block) {
    return (uint64_t *)(void *)(heap_user_ptr(block) + block->size);
}

static int heap_block_used(const heap_block_t *block) {
    return block && (block->flags & HEAP_BLOCK_USED);
}

static void heap_record_corruption(heap_block_t *block) {
    g_heap_corruption_events++;
    g_heap_last_error_addr = (uint64_t)(uintptr_t)block;
}

static void heap_mark_allocated(heap_block_t *block, size_t requested) {
    block->magic = HEAP_MAGIC;
    block->flags = HEAP_BLOCK_USED;
    block->requested = requested;
    block->front_guard = HEAP_FRONT_GUARD;
    *heap_tail_ptr(block) = HEAP_TAIL_GUARD;
    if (block->size) k_memset(heap_user_ptr(block), HEAP_POISON_ALLOC, (size_t)block->size);
}

static void heap_mark_freed(heap_block_t *block) {
    block->magic = HEAP_FREE_MAGIC;
    block->flags = 0;
    block->requested = 0;
    block->front_guard = HEAP_FRONT_GUARD;
    if (block->size) k_memset(heap_user_ptr(block), HEAP_POISON_FREE, (size_t)block->size);
}

static int heap_validate_block(heap_block_t *block) {
    if (!block) return 0;
    if (block->magic != HEAP_MAGIC || !heap_block_used(block)) {
        heap_record_corruption(block);
        return 0;
    }
    if (block->front_guard != HEAP_FRONT_GUARD) {
        heap_record_corruption(block);
        return 0;
    }
    if (*heap_tail_ptr(block) != HEAP_TAIL_GUARD) {
        heap_record_corruption(block);
        return 0;
    }
    return 1;
}

static uint64_t heap_scan_largest_free_block(void) {
    uint64_t largest = 0;
    heap_block_t *cur = g_heap_head;
    while (cur) {
        if (!heap_block_used(cur) && cur->size > largest) largest = cur->size;
        cur = cur->next;
    }
    return largest;
}

static uint64_t heap_scan_free_bytes(void) {
    uint64_t free_bytes = 0;
    heap_block_t *cur = g_heap_head;
    while (cur) {
        if (!heap_block_used(cur)) free_bytes += cur->size;
        cur = cur->next;
    }
    return free_bytes;
}

uint64_t kmalloc_early(uint64_t size) {
    if (!g_placement) g_placement = (((uint64_t)&_kernel_end) + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    {
        uint64_t addr = g_placement;
        g_placement = (g_placement + size + 15ULL) & ~15ULL;
        return addr;
    }
}

void heap_init(void) {
    g_heap_head = (heap_block_t *)(uintptr_t)HEAP_START;
    g_heap_head->magic = HEAP_FREE_MAGIC;
    g_heap_head->flags = 0;
    g_heap_head->size = HEAP_SIZE - BLOCK_HDR - BLOCK_FTR;
    g_heap_head->requested = 0;
    g_heap_head->front_guard = HEAP_FRONT_GUARD;
    g_heap_head->next = NULL;
    g_heap_head->prev = NULL;
    g_heap_alloc_count = 0;
    g_heap_free_count = 0;
    g_heap_failed_allocs = 0;
    g_heap_active_allocs = 0;
    g_heap_peak_used = 0;
    g_heap_corruption_events = 0;
    g_heap_last_error_addr = 0;
    if (g_heap_head->size) k_memset(heap_user_ptr(g_heap_head), HEAP_POISON_FREE, (size_t)g_heap_head->size);
}

void *kmalloc(size_t size) {
    heap_block_t *cur = g_heap_head;
    if (!size || !g_heap_head) return NULL;
    size = (size + 15u) & ~(size_t)15u;
    while (cur) {
        if (!heap_block_used(cur) && cur->size >= size) {
            if (cur->size >= size + BLOCK_HDR + BLOCK_FTR + 16ULL) {
                heap_block_t *nb = (heap_block_t *)(void *)(heap_user_ptr(cur) + size + BLOCK_FTR);
                nb->magic = HEAP_FREE_MAGIC;
                nb->flags = 0;
                nb->size = cur->size - size - BLOCK_HDR - BLOCK_FTR;
                nb->requested = 0;
                nb->front_guard = HEAP_FRONT_GUARD;
                nb->next = cur->next;
                nb->prev = cur;
                if (cur->next) cur->next->prev = nb;
                cur->next = nb;
                cur->size = size;
                if (nb->size) k_memset(heap_user_ptr(nb), HEAP_POISON_FREE, (size_t)nb->size);
            }
            heap_mark_allocated(cur, size);
            g_heap_alloc_count++;
            g_heap_active_allocs++;
            {
                uint64_t used = heap_used();
                if (used > g_heap_peak_used) g_heap_peak_used = used;
            }
            return (void *)heap_user_ptr(cur);
        }
        cur = cur->next;
    }
    g_heap_failed_allocs++;
    return NULL;
}

void *kcalloc(size_t count, size_t size) {
    void *ptr = kmalloc(count * size);
    if (ptr) __builtin_memset(ptr, 0, count * size);
    return ptr;
}

void *krealloc(void *ptr, size_t size) {
    heap_block_t *block;
    void *new_ptr;
    if (!ptr) return kmalloc(size);
    if (!size) {
        kfree(ptr);
        return NULL;
    }
    block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HDR);
    if (!heap_validate_block(block)) return NULL;
    if (block->size >= size) {
        block->requested = size;
        *heap_tail_ptr(block) = HEAP_TAIL_GUARD;
        return ptr;
    }
    new_ptr = kmalloc(size);
    if (new_ptr) {
        __builtin_memcpy(new_ptr, ptr, block->requested ? block->requested : block->size);
        kfree(ptr);
    }
    return new_ptr;
}

void kfree(void *ptr) {
    heap_block_t *block;
    if (!ptr) return;
    block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HDR);
    if (block->magic == HEAP_FREE_MAGIC && !heap_block_used(block)) {
        heap_record_corruption(block);
        return;
    }
    if (!heap_validate_block(block)) return;
    heap_mark_freed(block);
    g_heap_free_count++;
    if (g_heap_active_allocs) g_heap_active_allocs--;
    if (block->next && !heap_block_used(block->next)) {
        block->size += BLOCK_HDR + BLOCK_FTR + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }
    if (block->prev && !heap_block_used(block->prev)) {
        block->prev->size += BLOCK_HDR + BLOCK_FTR + block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
        block = block->prev;
    }
    if (block->size) k_memset(heap_user_ptr(block), HEAP_POISON_FREE, (size_t)block->size);
}

uint64_t heap_used(void) {
    uint64_t used = 0;
    heap_block_t *cur = g_heap_head;
    while (cur) {
        if (heap_block_used(cur)) used += cur->size + BLOCK_HDR + BLOCK_FTR;
        cur = cur->next;
    }
    return used;
}

uint64_t heap_total(void) {
    return HEAP_SIZE;
}

uint64_t heap_free_bytes(void) {
    return heap_scan_free_bytes();
}

uint64_t heap_allocation_count(void) {
    return g_heap_alloc_count;
}

uint64_t heap_free_count(void) {
    return g_heap_free_count;
}

uint64_t heap_failed_allocations(void) {
    return g_heap_failed_allocs;
}

uint64_t heap_active_allocations(void) {
    return g_heap_active_allocs;
}

uint64_t heap_corruption_events(void) {
    return g_heap_corruption_events;
}

void heap_debug_report(char *buf, int max) {
    char nb[48];
    char hex[32];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    heap_cat(buf, "HeapGuard: enabled\n", max);
    heap_cat(buf, "Allocator: first-fit + coalescing + tail canaries\n", max);
    heap_cat(buf, "UnderflowGuard: header/front guard active\n", max);
    heap_cat(buf, "OverflowGuard: tail canary active\n", max);
    heap_cat(buf, "LeakTelemetry: allocation/free counters active\n", max);

    heap_cat(buf, "HeapUsedBytes=", max); heap_u64(heap_used(), nb, sizeof(nb)); heap_cat(buf, nb, max); heap_cat(buf, "\n", max);
    heap_cat(buf, "HeapFreeBytes=", max); heap_u64(heap_free_bytes(), nb, sizeof(nb)); heap_cat(buf, nb, max); heap_cat(buf, "\n", max);
    heap_cat(buf, "LargestFreeBlock=", max); heap_u64(heap_scan_largest_free_block(), nb, sizeof(nb)); heap_cat(buf, nb, max); heap_cat(buf, "\n", max);
    heap_cat(buf, "AllocationCount=", max); heap_u64(g_heap_alloc_count, nb, sizeof(nb)); heap_cat(buf, nb, max); heap_cat(buf, "\n", max);
    heap_cat(buf, "FreeCount=", max); heap_u64(g_heap_free_count, nb, sizeof(nb)); heap_cat(buf, nb, max); heap_cat(buf, "\n", max);
    heap_cat(buf, "ActiveAllocations=", max); heap_u64(g_heap_active_allocs, nb, sizeof(nb)); heap_cat(buf, nb, max); heap_cat(buf, "\n", max);
    heap_cat(buf, "FailedAllocations=", max); heap_u64(g_heap_failed_allocs, nb, sizeof(nb)); heap_cat(buf, nb, max); heap_cat(buf, "\n", max);
    heap_cat(buf, "PeakUsedBytes=", max); heap_u64(g_heap_peak_used, nb, sizeof(nb)); heap_cat(buf, nb, max); heap_cat(buf, "\n", max);
    heap_cat(buf, "CorruptionEvents=", max); heap_u64(g_heap_corruption_events, nb, sizeof(nb)); heap_cat(buf, nb, max); heap_cat(buf, "\n", max);
    heap_cat(buf, "LeakSuspected=", max); heap_cat(buf, g_heap_active_allocs ? "yes" : "no", max); heap_cat(buf, "\n", max);
    heap_cat(buf, "LastErrorBlock=", max); heap_hex(g_heap_last_error_addr, hex, sizeof(hex)); heap_cat(buf, hex, max); heap_cat(buf, "\n", max);
}
