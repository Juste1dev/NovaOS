#ifndef LIBC_H
#define LIBC_H
#include <stdint.h>
#include <stddef.h>

int    k_strcmp(const char *a, const char *b);
int    k_strncmp(const char *a, const char *b, size_t n);
char*  k_strcpy(char *dst, const char *src);
char*  k_strncpy(char *dst, const char *src, size_t n);
size_t k_strlen(const char *s);
void*  k_memset(void *dst, int c, size_t n);
void*  k_memcpy(void *dst, const void *src, size_t n);
void*  k_memmove(void *dst, const void *src, size_t n);
int    k_memcmp(const void *a, const void *b, size_t n);
char*  k_strcat(char *dst, const char *src);
char*  k_strchr(const char *s, int c);
char*  k_strstr(const char *haystack, const char *needle);

#define __builtin_strcmp   k_strcmp
#define __builtin_strncmp  k_strncmp
#define __builtin_memset   k_memset
#define __builtin_memcpy   k_memcpy
#define __builtin_memmove  k_memmove
#define __builtin_strlen   k_strlen
#define __builtin_strcpy   k_strcpy

#endif
