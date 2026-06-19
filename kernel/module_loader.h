#ifndef NOVA_MODULE_LOADER_H
#define NOVA_MODULE_LOADER_H

#include <stdint.h>

#define MODULE_MAX 12

typedef struct {
    char name[32];
    char path[96];
    char type[24];
    int  size;
    int  builtin;
    int  loaded;
    int  hotplug;
    int  refcount;
} nova_module_t;

void module_loader_init(void);
void module_loader_report(char *buf, int max);
void module_loader_hotplug_report(char *buf, int max);
int  module_insmod(const char *name_or_path, char *out, int max);
int  module_rmmod(const char *name, char *out, int max);
int  module_modprobe(const char *name, char *out, int max);

#endif
