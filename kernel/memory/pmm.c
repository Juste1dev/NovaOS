#include "../memory.h"
#include <stdint.h>
#include <stddef.h>

#define PMM_BITMAP_SIZE 32768U
#define PMM_MAX_FRAMES  ((uint64_t)PMM_BITMAP_SIZE * 32ULL)

typedef struct {
    uint64_t highest_frame;
    uint64_t usable_frames;
    uint64_t used_frames;
    uint64_t reserve_ops;
    uint64_t alloc_failures;
} pmm_state_t;

static uint32_t g_pmm_bitmap[PMM_BITMAP_SIZE];
static pmm_state_t g_pmm;

static void pmm_clear(uint64_t bit) {
    g_pmm_bitmap[bit / 32ULL] &= ~(1u << (bit % 32ULL));
}

static void pmm_set(uint64_t bit) {
    g_pmm_bitmap[bit / 32ULL] |= (1u << (bit % 32ULL));
}

static int pmm_test(uint64_t bit) {
    return (g_pmm_bitmap[bit / 32ULL] & (1u << (bit % 32ULL))) != 0;
}

static uint64_t pmm_align_down(uint64_t value) {
    return value & ~(PAGE_SIZE - 1ULL);
}

static uint64_t pmm_align_up(uint64_t value) {
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static int pmm_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void pmm_cat(char *dst, const char *src, int max) {
    int dl = pmm_len(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void pmm_u64(uint64_t value, char *buf, int max) {
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

static uint64_t pmm_find_largest_free_run(void) {
    uint64_t best = 0;
    uint64_t run = 0;
    for (uint64_t frame = 0; frame <= g_pmm.highest_frame && frame < PMM_MAX_FRAMES; ++frame) {
        if (!pmm_test(frame)) {
            run++;
            if (run > best) best = run;
        } else {
            run = 0;
        }
    }
    return best;
}

void pmm_init(uint64_t mem_upper) {
    uint64_t highest_byte = (mem_upper + 1024ULL) * 1024ULL;
    uint64_t frames = pmm_align_up(highest_byte) / PAGE_SIZE;
    if (frames > PMM_MAX_FRAMES) frames = PMM_MAX_FRAMES;

    for (uint32_t i = 0; i < PMM_BITMAP_SIZE; ++i) g_pmm_bitmap[i] = 0xFFFFFFFFu;
    g_pmm.highest_frame = frames ? (frames - 1ULL) : 0ULL;
    g_pmm.usable_frames = 0;
    g_pmm.used_frames = 0;
    g_pmm.reserve_ops = 0;
    g_pmm.alloc_failures = 0;
}

void pmm_init_region(uint64_t base, uint64_t size) {
    uint64_t start = pmm_align_up(base);
    uint64_t end = pmm_align_down(base + size);
    if (end <= start) return;

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint64_t frame = addr / PAGE_SIZE;
        if (frame >= PMM_MAX_FRAMES) break;
        if (frame > g_pmm.highest_frame) g_pmm.highest_frame = frame;
        if (pmm_test(frame)) {
            pmm_clear(frame);
            g_pmm.usable_frames++;
        }
    }

    if (!pmm_test(0)) {
        pmm_set(0);
        if (g_pmm.usable_frames) g_pmm.usable_frames--;
        g_pmm.used_frames++;
    }
}

void pmm_reserve_region(uint64_t base, uint64_t size) {
    uint64_t start = pmm_align_down(base);
    uint64_t end = pmm_align_up(base + size);
    if (!size || end <= start) return;

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint64_t frame = addr / PAGE_SIZE;
        if (frame >= PMM_MAX_FRAMES) break;
        if (!pmm_test(frame)) {
            pmm_set(frame);
            g_pmm.used_frames++;
        }
    }
    g_pmm.reserve_ops++;
}

uint64_t pmm_alloc_block(void) {
    for (uint64_t i = 0; i <= g_pmm.highest_frame && i < PMM_MAX_FRAMES; ++i) {
        if (!pmm_test(i)) {
            pmm_set(i);
            g_pmm.used_frames++;
            return i * PAGE_SIZE;
        }
    }
    g_pmm.alloc_failures++;
    return 0;
}

void pmm_free_block(uint64_t addr) {
    uint64_t frame = addr / PAGE_SIZE;
    if (frame >= PMM_MAX_FRAMES) return;
    if (pmm_test(frame)) {
        pmm_clear(frame);
        if (g_pmm.used_frames) g_pmm.used_frames--;
    }
}

uint64_t pmm_total_blocks(void) {
    return g_pmm.usable_frames;
}

uint64_t pmm_used_blocks(void) {
    return g_pmm.used_frames <= g_pmm.usable_frames ? g_pmm.used_frames : g_pmm.usable_frames;
}

uint64_t pmm_available_blocks(void) {
    uint64_t used = pmm_used_blocks();
    return g_pmm.usable_frames > used ? (g_pmm.usable_frames - used) : 0ULL;
}

void pmm_report(char *buf, int max) {
    char nb[48];
    if (!buf || max <= 0) return;
    buf[0] = 0;
    pmm_cat(buf, "PMM: bitmap allocator\n", max);
    pmm_cat(buf, "PageSize=4096\n", max);
    pmm_cat(buf, "UsableFrames=", max); pmm_u64(g_pmm.usable_frames, nb, sizeof(nb)); pmm_cat(buf, nb, max); pmm_cat(buf, "\n", max);
    pmm_cat(buf, "UsedFrames=", max); pmm_u64(pmm_used_blocks(), nb, sizeof(nb)); pmm_cat(buf, nb, max); pmm_cat(buf, "\n", max);
    pmm_cat(buf, "FreeFrames=", max); pmm_u64(pmm_available_blocks(), nb, sizeof(nb)); pmm_cat(buf, nb, max); pmm_cat(buf, "\n", max);
    pmm_cat(buf, "LargestFreeRunFrames=", max); pmm_u64(pmm_find_largest_free_run(), nb, sizeof(nb)); pmm_cat(buf, nb, max); pmm_cat(buf, "\n", max);
    pmm_cat(buf, "ReserveOps=", max); pmm_u64(g_pmm.reserve_ops, nb, sizeof(nb)); pmm_cat(buf, nb, max); pmm_cat(buf, "\n", max);
    pmm_cat(buf, "AllocFailures=", max); pmm_u64(g_pmm.alloc_failures, nb, sizeof(nb)); pmm_cat(buf, nb, max); pmm_cat(buf, "\n", max);
}
