#include "libc.h"
#include <stddef.h>
#include <stdint.h>

int k_strcmp(const char *a, const char *b) {
    while (*a && *b && *a==*b){a++;b++;}
    return (unsigned char)*a-(unsigned char)*b;
}

int k_strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *b && *a==*b){a++;b++;n--;}
    if (!n) return 0;
    return (unsigned char)*a-(unsigned char)*b;
}

char* k_strcpy(char *dst, const char *src) {
    char *d=dst; while(*src)*d++=*src++; *d=0; return dst;
}

char* k_strncpy(char *dst, const char *src, size_t n) {
    size_t i=0;
    for(;i<n&&src[i];i++) dst[i]=src[i];
    for(;i<n;i++) dst[i]=0;
    return dst;
}

size_t k_strlen(const char *s) {
    size_t n=0; while(s[n])n++; return n;
}

void* k_memset(void *dst, int c, size_t n) {
    uint8_t *d=dst; while(n--)*d++=(uint8_t)c; return dst;
}

void* k_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d=dst; const uint8_t *s=src; while(n--)*d++=*s++; return dst;
}

void* k_memmove(void *dst, const void *src, size_t n) {
    uint8_t *d=dst; const uint8_t *s=src;
    if(d<s){while(n--)*d++=*s++;}
    else{d+=n;s+=n;while(n--)*--d=*--s;}
    return dst;
}

int k_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x=a,*y=b;
    while(n--){if(*x!=*y)return *x-*y;x++;y++;}
    return 0;
}

char* k_strcat(char *dst, const char *src) {
    char *d=dst; while(*d)d++; while(*src)*d++=*src++; *d=0; return dst;
}

char* k_strchr(const char *s, int c) {
    while(*s){if(*s==(char)c)return (char*)s;s++;} return NULL;
}

char* k_strstr(const char *h, const char *n) {
    if(!*n)return(char*)h;
    while(*h){
        const char *p=h,*q=n;
        while(*p&&*q&&*p==*q){p++;q++;}
        if(!*q)return(char*)h;
        h++;
    }
    return NULL;
}

void* memcpy(void *dst, const void *src, __SIZE_TYPE__ n) {
    return k_memcpy(dst, src, n);
}
void* memset(void *dst, int c, __SIZE_TYPE__ n) {
    return k_memset(dst, c, n);
}
void* memmove(void *dst, const void *src, __SIZE_TYPE__ n) {
    return k_memmove(dst, src, n);
}
int memcmp(const void *a, const void *b, __SIZE_TYPE__ n) {
    return k_memcmp(a, b, n);
}
__SIZE_TYPE__ strlen(const char *s) {
    return k_strlen(s);
}
