#ifndef NOVA_PKG_H
#define NOVA_PKG_H

#include <stdint.h>

#define NOVA_SHORTCUT_NAME_MAX   64
#define NOVA_SHORTCUT_TARGET_MAX 256
#define NOVA_SHORTCUT_TYPE_MAX   16
#define NOVA_SHORTCUT_ID_MAX     64

typedef struct {
    char package_id[NOVA_SHORTCUT_ID_MAX];
    char name[NOVA_SHORTCUT_NAME_MAX];
    char launch_type[NOVA_SHORTCUT_TYPE_MAX];
    char launch_target[NOVA_SHORTCUT_TARGET_MAX];
} nova_shortcut_t;

int nova_pkg_install_from_text(const char *text, char *status, int status_max);
int nova_pkg_remove(const char *package_id, char *status, int status_max);
int nova_pkg_query_installed(char *out, int max);
int nova_pkg_load_shortcuts(const char *dir, nova_shortcut_t *out, int max);
int nova_store_download_package(const char *app_id, char *out, int max, char *meta, int meta_max);
int nova_store_query(const char *command, const char *arg, char *out, int max, char *meta, int meta_max);
int nova_store_self_test(char *report, int report_max);

#endif
