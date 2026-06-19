#ifndef NOVA_DRIVER_MANAGER_H
#define NOVA_DRIVER_MANAGER_H

#include <stdint.h>

#define NOVA_MAX_DRIVERS 24

typedef enum {
    NOVA_BUS_PLATFORM = 0,
    NOVA_BUS_PCI,
    NOVA_BUS_USB,
    NOVA_BUS_VIRTUAL
} nova_driver_bus_t;

typedef struct {
    int      loaded;
    int      builtin;
    int      hotplug;
    uint8_t  irq;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    char     name[32];
    char     category[16];
    char     version[16];
    char     binding[32];
    char     device_path[48];
    char     summary[96];
    nova_driver_bus_t bus;
} nova_driver_info_t;

void driver_manager_init(void);
int  driver_manager_count(void);
const nova_driver_info_t* driver_manager_get(int index);
void driver_manager_report(char *buf, int max);

#endif
