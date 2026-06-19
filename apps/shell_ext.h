#ifndef NOVA_SHELL_EXT_H
#define NOVA_SHELL_EXT_H

#include <stdint.h>

typedef struct {
    void (*println)(void *ctx, const char *text);
    void (*println_color)(void *ctx, const char *text, uint32_t color);
    void (*resolve_path)(void *ctx, const char *input, char *out, int max);
    const char *(*get_cwd)(void *ctx);
    const char *(*get_username)(void *ctx);
    void (*set_cwd)(void *ctx, const char *cwd);
    void (*set_username)(void *ctx, const char *username);
} shell_ext_api_t;

int shell_ext_try_handle(void *ctx, const shell_ext_api_t *api, const char *cmd_line);

#endif
