#ifndef NOVA_PCI_H
#define NOVA_PCI_H

#include <stdint.h>

#define NOVA_PCI_MAX_DEVICES 64

typedef struct {
    int      present;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  irq_line;
    char     label[48];
    char     binding[32];
} nova_pci_device_t;

void pci_init(void);
int  pci_device_count(void);
const nova_pci_device_t* pci_get_device(int index);
int  pci_has_class(uint8_t class_code, uint8_t subclass);
const nova_pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass, int ordinal);
void pci_describe_all(char *buf, int max);

#endif
