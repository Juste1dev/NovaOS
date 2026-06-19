

#ifndef STDLIB_H
#define STDLIB_H

#include <stdint.h>
#include <stddef.h>

static inline int __attribute__((unused)) __builtin_strcmp_impl(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline size_t __attribute__((unused)) __builtin_strlen_impl(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static inline void* __attribute__((unused)) __builtin_memset_impl(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

static inline void* __attribute__((unused)) __builtin_memcpy_impl(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

uint32_t heap_used(void);
uint32_t heap_total(void);

#endif
