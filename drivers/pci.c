#include "pci.h"
#include "../libc.h"
#include <stddef.h>
#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

static nova_pci_device_t g_pci_devices[NOVA_PCI_MAX_DEVICES];
static int g_pci_count = 0;
static int g_pci_ready = 0;

static inline void pci_outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port));
}

static inline uint32_t pci_inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1,%0":"=a"(value):"Nd"(port));
    return value;
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000u
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)function << 8)
                     | ((uint32_t)offset & 0xFCu);
    pci_outl(PCI_CONFIG_ADDR, address);
    return pci_inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t value = pci_read32(bus, slot, function, offset);
    return (uint16_t)((value >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t value = pci_read32(bus, slot, function, offset);
    return (uint8_t)((value >> ((offset & 3u) * 8u)) & 0xFFu);
}

static void pci_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void pci_cat(char *dst, const char *src, int max) {
    int dl = (int)k_strlen(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void pci_hex8(uint8_t value, char *buf) {
    static const char hex[] = "0123456789abcdef";
    buf[0] = hex[(value >> 4) & 0xFu];
    buf[1] = hex[value & 0xFu];
    buf[2] = 0;
}

static void pci_hex16(uint16_t value, char *buf) {
    static const char hex[] = "0123456789abcdef";
    buf[0] = hex[(value >> 12) & 0xFu];
    buf[1] = hex[(value >> 8) & 0xFu];
    buf[2] = hex[(value >> 4) & 0xFu];
    buf[3] = hex[value & 0xFu];
    buf[4] = 0;
}

static const char *pci_vendor_name(uint16_t vendor_id) {
    switch (vendor_id) {
        case 0x8086: return "Intel";
        case 0x1234: return "QEMU/Bochs";
        case 0x1AF4: return "Virtio";
        case 0x10EC: return "Realtek";
        case 0x1022: return "AMD";
        case 0x1B36: return "Red Hat/QEMU";
        default: return "Unknown";
    }
}

static const char *pci_class_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    (void)prog_if;
    switch (class_code) {
        case 0x01:
            switch (subclass) {
                case 0x01: return "IDE storage";
                case 0x06: return "SATA AHCI";
                case 0x08: return "NVMe";
                default: return "Storage";
            }
        case 0x02: return "Network";
        case 0x03: return subclass == 0x00 ? "VGA display" : "Display";
        case 0x04: return "Multimedia";
        case 0x06:
            switch (subclass) {
                case 0x00: return "Host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI bridge";
                case 0x80: return "Bridge";
                default: return "Bridge";
            }
        case 0x0C:
            if (subclass == 0x03 && prog_if == 0x30) return "USB xHCI";
            if (subclass == 0x03 && prog_if == 0x20) return "USB EHCI";
            if (subclass == 0x03 && prog_if == 0x10) return "USB OHCI";
            if (subclass == 0x03 && prog_if == 0x00) return "USB UHCI";
            return "Serial bus";
        default: return "Generic";
    }
}

static const char *pci_binding_name(uint16_t vendor_id, uint16_t device_id, uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    if (vendor_id == 0x1234u && device_id == 0x1111u) return "qemu-stdvga";
    if (vendor_id == 0x8086u && device_id == 0x1237u) return "i440fx-host";
    if (vendor_id == 0x8086u && device_id == 0x7000u) return "piix3-isa";
    if (vendor_id == 0x8086u && device_id == 0x7010u) return "piix3-ide";
    if (vendor_id == 0x8086u && device_id == 0x7113u) return "piix4-acpi";
    if (vendor_id == 0x10ECu && device_id == 0x8139u) return "rtl8139";
    if (class_code == 0x0Cu && subclass == 0x03u && prog_if == 0x30u) return "xhci";
    if (class_code == 0x01u && subclass == 0x06u) return "ahci";
    if (class_code == 0x01u && subclass == 0x08u) return "nvme";
    if (class_code == 0x01u && subclass == 0x01u) return "ata";
    if (class_code == 0x02u && subclass == 0x00u) return "ethernet";
    if (class_code == 0x03u && subclass == 0x00u) return "vga";
    return "generic";
}

static void pci_make_label(nova_pci_device_t *dev) {
    char ven[5];
    char did[5];
    pci_hex16(dev->vendor_id, ven);
    pci_hex16(dev->device_id, did);
    pci_copy(dev->label, pci_vendor_name(dev->vendor_id), sizeof(dev->label));
    pci_cat(dev->label, " ", sizeof(dev->label));
    pci_cat(dev->label, pci_class_name(dev->class_code, dev->subclass, dev->prog_if), sizeof(dev->label));
    pci_cat(dev->label, " ", sizeof(dev->label));
    pci_cat(dev->label, ven, sizeof(dev->label));
    pci_cat(dev->label, ":", sizeof(dev->label));
    pci_cat(dev->label, did, sizeof(dev->label));
    pci_copy(dev->binding, pci_binding_name(dev->vendor_id, dev->device_id, dev->class_code, dev->subclass, dev->prog_if), sizeof(dev->binding));
}

static void pci_add_device(uint8_t bus, uint8_t slot, uint8_t function) {
    nova_pci_device_t *dev;
    uint16_t vendor_id;
    if (g_pci_count >= NOVA_PCI_MAX_DEVICES) return;
    vendor_id = pci_read16(bus, slot, function, 0x00);
    if (vendor_id == 0xFFFFu) return;
    dev = &g_pci_devices[g_pci_count++];
    k_memset(dev, 0, sizeof(*dev));
    dev->present = 1;
    dev->bus = bus;
    dev->slot = slot;
    dev->function = function;
    dev->vendor_id = vendor_id;
    dev->device_id = pci_read16(bus, slot, function, 0x02);
    dev->revision = pci_read8(bus, slot, function, 0x08);
    dev->prog_if = pci_read8(bus, slot, function, 0x09);
    dev->subclass = pci_read8(bus, slot, function, 0x0A);
    dev->class_code = pci_read8(bus, slot, function, 0x0B);
    dev->header_type = (uint8_t)(pci_read8(bus, slot, function, 0x0E) & 0x7Fu);
    dev->irq_line = pci_read8(bus, slot, function, 0x3C);
    pci_make_label(dev);
}

void pci_init(void) {
    if (g_pci_ready) return;
    k_memset(g_pci_devices, 0, sizeof(g_pci_devices));
    g_pci_count = 0;
    for (uint16_t bus = 0; bus < 32; ++bus) {
        for (uint16_t slot = 0; slot < 32; ++slot) {
            uint16_t vendor = pci_read16((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            uint8_t fn_limit = 1;
            if (vendor == 0xFFFFu) continue;
            if (pci_read8((uint8_t)bus, (uint8_t)slot, 0, 0x0E) & 0x80u) fn_limit = 8;
            for (uint8_t function = 0; function < fn_limit; ++function) {
                pci_add_device((uint8_t)bus, (uint8_t)slot, function);
            }
        }
    }
    g_pci_ready = 1;
}

int pci_device_count(void) {
    if (!g_pci_ready) pci_init();
    return g_pci_count;
}

const nova_pci_device_t* pci_get_device(int index) {
    if (!g_pci_ready) pci_init();
    if (index < 0 || index >= g_pci_count) return NULL;
    return &g_pci_devices[index];
}

int pci_has_class(uint8_t class_code, uint8_t subclass) {
    if (!g_pci_ready) pci_init();
    for (int i = 0; i < g_pci_count; ++i) {
        if (g_pci_devices[i].class_code == class_code && g_pci_devices[i].subclass == subclass) return 1;
    }
    return 0;
}

const nova_pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass, int ordinal) {
    int found = 0;
    if (!g_pci_ready) pci_init();
    for (int i = 0; i < g_pci_count; ++i) {
        if (g_pci_devices[i].class_code == class_code && g_pci_devices[i].subclass == subclass) {
            if (found == ordinal) return &g_pci_devices[i];
            found++;
        }
    }
    return NULL;
}

void pci_describe_all(char *buf, int max) {
    if (!buf || max <= 0) return;
    if (!g_pci_ready) pci_init();
    k_memset(buf, 0, (size_t)max);
    pci_cat(buf, "BDF IRQ VENDOR:DEVICE CLASS BINDING LABEL\n", max);
    pci_cat(buf, "------------------------------------------\n", max);
    for (int i = 0; i < g_pci_count; ++i) {
        char line[192];
        char b[3], s[3], f[3], ven[5], did[5], irq[3], cls[3], sub[3];
        const nova_pci_device_t *dev = &g_pci_devices[i];
        k_memset(line, 0, sizeof(line));
        pci_hex8(dev->bus, b); pci_hex8(dev->slot, s); pci_hex8(dev->function, f);
        pci_hex16(dev->vendor_id, ven); pci_hex16(dev->device_id, did);
        pci_hex8(dev->irq_line, irq); pci_hex8(dev->class_code, cls); pci_hex8(dev->subclass, sub);
        pci_cat(line, b, sizeof(line));
        pci_cat(line, ":", sizeof(line));
        pci_cat(line, s, sizeof(line));
        pci_cat(line, ".", sizeof(line));
        pci_cat(line, f, sizeof(line));
        pci_cat(line, " irq=", sizeof(line));
        pci_cat(line, irq, sizeof(line));
        pci_cat(line, " ", sizeof(line));
        pci_cat(line, ven, sizeof(line));
        pci_cat(line, ":", sizeof(line));
        pci_cat(line, did, sizeof(line));
        pci_cat(line, " class=", sizeof(line));
        pci_cat(line, cls, sizeof(line));
        pci_cat(line, "/", sizeof(line));
        pci_cat(line, sub, sizeof(line));
        pci_cat(line, " bind=", sizeof(line));
        pci_cat(line, dev->binding, sizeof(line));
        pci_cat(line, " ", sizeof(line));
        pci_cat(line, dev->label, sizeof(line));
        pci_cat(line, "\n", sizeof(line));
        pci_cat(buf, line, max);
    }
}
