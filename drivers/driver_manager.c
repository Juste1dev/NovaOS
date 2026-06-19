#include "driver_manager.h"
#include "pci.h"
#include "vbe.h"
#include "sound.h"
#include "../fs/vfs.h"
#include "../net/net.h"
#include "../libc.h"
#include <stddef.h>
#include <stdint.h>

static nova_driver_info_t g_drivers[NOVA_MAX_DRIVERS];
static int g_driver_count = 0;
static int g_driver_manager_ready = 0;

static int dm_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void dm_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void dm_cat(char *dst, const char *src, int max) {
    int dl = dm_len(dst);
    int i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) {
        dst[dl + i] = src[i];
        i++;
    }
    dst[dl + i] = 0;
}

static void dm_line(char *dst, const char *src, int max) {
    dm_cat(dst, src, max);
    dm_cat(dst, "\n", max);
}

static void dm_u32(uint32_t value, char *buf, int max) {
    char tmp[16];
    int pos = 0;
    int out = 0;
    if (!buf || max <= 0) return;
    if (!value) {
        buf[0] = '0';
        if (max > 1) buf[1] = 0;
        return;
    }
    while (value && pos < (int)sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}

static void dm_hex8(uint8_t value, char *buf) {
    static const char hex[] = "0123456789abcdef";
    buf[0] = hex[(value >> 4) & 0xFu];
    buf[1] = hex[value & 0xFu];
    buf[2] = 0;
}

static void dm_hex16(uint16_t value, char *buf) {
    static const char hex[] = "0123456789abcdef";
    buf[0] = hex[(value >> 12) & 0xFu];
    buf[1] = hex[(value >> 8) & 0xFu];
    buf[2] = hex[(value >> 4) & 0xFu];
    buf[3] = hex[value & 0xFu];
    buf[4] = 0;
}

static const char *dm_bus_name(nova_driver_bus_t bus) {
    switch (bus) {
        case NOVA_BUS_PCI: return "pci";
        case NOVA_BUS_USB: return "usb";
        case NOVA_BUS_VIRTUAL: return "virtual";
        default: return "platform";
    }
}

static void dm_ensure_dir(const char *path) {
    if (!vfs_exists(path) || !vfs_is_dir(path)) (void)vfs_mkdir(path);
}

static int dm_register_driver(const char *name,
                              const char *category,
                              const char *version,
                              const char *binding,
                              const char *device_path,
                              const char *summary,
                              nova_driver_bus_t bus,
                              int builtin,
                              int hotplug,
                              uint8_t irq,
                              uint16_t vendor_id,
                              uint16_t device_id,
                              uint8_t class_code,
                              uint8_t subclass) {
    nova_driver_info_t *drv;
    if (g_driver_count >= NOVA_MAX_DRIVERS) return -1;
    drv = &g_drivers[g_driver_count++];
    k_memset(drv, 0, sizeof(*drv));
    drv->loaded = 1;
    drv->builtin = builtin;
    drv->hotplug = hotplug;
    drv->irq = irq;
    drv->vendor_id = vendor_id;
    drv->device_id = device_id;
    drv->class_code = class_code;
    drv->subclass = subclass;
    drv->bus = bus;
    dm_copy(drv->name, name, sizeof(drv->name));
    dm_copy(drv->category, category, sizeof(drv->category));
    dm_copy(drv->version, version, sizeof(drv->version));
    dm_copy(drv->binding, binding, sizeof(drv->binding));
    dm_copy(drv->device_path, device_path, sizeof(drv->device_path));
    dm_copy(drv->summary, summary, sizeof(drv->summary));
    return g_driver_count - 1;
}

static void dm_register_core_drivers(void) {
    dm_register_driver("framebuffer-vbe", "display", "1.0", "vbe", "/dev/fb0",
                       "Framebuffer VBE double buffering 32bpp", NOVA_BUS_PLATFORM, 1, 0, 0,
                       0, 0, 0x03, 0x00);
    dm_register_driver("ps2-keyboard", "input", "1.0", "ps2", "/dev/input/keyboard0",
                       "Clavier systeme et console tty0", NOVA_BUS_PLATFORM, 1, 0, 1,
                       0, 0, 0x09, 0x80);
    dm_register_driver("ps2-mouse", "input", "1.1", "imps2", "/dev/input/mouse0",
                       "Souris avec molette et lissage", NOVA_BUS_PLATFORM, 1, 0, 12,
                       0, 0, 0x09, 0x80);
    dm_register_driver("pit-timer", "system", "1.0", "pit-1000hz", "/dev/clock0",
                       "Horloge PIT et base du scheduler preemptif", NOVA_BUS_PLATFORM, 1, 0, 0,
                       0, 0, 0x08, 0x00);
    dm_register_driver(sound_sb16_detected() ? "audio-core-sb16" : "audio-core-pcspk",
                       "audio", "1.0",
                       sound_sb16_detected() ? "sb16+pcspk" : "pcspk",
                       "/dev/snd/pcmC0D0p",
                       sound_output_backend(),
                       NOVA_BUS_PLATFORM, 1, 0, sound_sb16_detected() ? 5 : 0,
                       0, 0, 0x04, 0x01);
    dm_register_driver("net-eth0", "network", "1.0", "nova-net", "/dev/net/eth0",
                       net_eth0.connected ? "Interface reseau principale active" : "Interface reseau profil standby",
                       NOVA_BUS_VIRTUAL, 1, 0, 0,
                       0, 0, 0x02, 0x00);
    if (net_wlan0.wifi_enabled) {
        dm_register_driver("net-wlan0", "network", "1.0", "nova-wifi", "/dev/net/wlan0",
                           net_wlan0.connected ? "Profil Wi-Fi connecte" : "Profil Wi-Fi pret",
                           NOVA_BUS_VIRTUAL, 1, 1, 0,
                           0, 0, 0x02, 0x80);
    }
}

static void dm_register_pci_backed_drivers(void) {
    const nova_pci_device_t *dev;
    dev = pci_find_class(0x01, 0x01, 0);
    if (dev) {
        dm_register_driver("ata-controller", "storage", "1.0", dev->binding, "/dev/disk0",
                           dev->label, NOVA_BUS_PCI, 1, 0, dev->irq_line,
                           dev->vendor_id, dev->device_id, dev->class_code, dev->subclass);
    }
    dev = pci_find_class(0x0C, 0x03, 0);
    if (dev) {
        dm_register_driver("usb-host", "usb", "1.0", dev->binding, "/dev/bus/usb0",
                           "Controleur USB detecte pour hotplug et HID", NOVA_BUS_PCI, 1, 1, dev->irq_line,
                           dev->vendor_id, dev->device_id, dev->class_code, dev->subclass);
        dm_register_driver("usb-hid", "input", "1.0", "hid-generic", "/dev/input/usbkbd0",
                           "Pont logique clavier/souris USB pour QEMU", NOVA_BUS_USB, 1, 1, dev->irq_line,
                           dev->vendor_id, dev->device_id, 0x03, 0x01);
    }
    dev = pci_find_class(0x03, 0x00, 0);
    if (dev) {
        dm_register_driver("gpu-basic", "display", "1.0", dev->binding, "/dev/fb0",
                           dev->label, NOVA_BUS_PCI, 1, 0, dev->irq_line,
                           dev->vendor_id, dev->device_id, dev->class_code, dev->subclass);
    }
    dev = pci_find_class(0x02, 0x00, 0);
    if (dev) {
        dm_register_driver("nic-pci", "network", "1.0", dev->binding, "/dev/net/eth0",
                           dev->label, NOVA_BUS_PCI, 1, 0, dev->irq_line,
                           dev->vendor_id, dev->device_id, dev->class_code, dev->subclass);
    }
}

static void dm_publish_device_nodes(void) {
    char fb_meta[256];
    char net_meta[256];
    char ip[20];
    char mac[20];
    char audio_meta[256];

    dm_ensure_dir("/dev");
    dm_ensure_dir("/dev/input");
    dm_ensure_dir("/dev/net");
    dm_ensure_dir("/dev/snd");
    dm_ensure_dir("/dev/bus");

    (void)vfs_write_file("/dev/tty0", "type=tty\nrole=system-console\n", (uint32_t)dm_len("type=tty\nrole=system-console\n"));
    (void)vfs_write_file("/dev/null", "type=null\n", (uint32_t)dm_len("type=null\n"));
    (void)vfs_write_file("/dev/random", "type=random\nquality=pseudo\n", (uint32_t)dm_len("type=random\nquality=pseudo\n"));
    (void)vfs_write_file("/dev/input/keyboard0", "type=input\nprotocol=ps2\nrole=keyboard\n", (uint32_t)dm_len("type=input\nprotocol=ps2\nrole=keyboard\n"));
    (void)vfs_write_file("/dev/input/mouse0", "type=input\nprotocol=imps2\nrole=mouse\n", (uint32_t)dm_len("type=input\nprotocol=imps2\nrole=mouse\n"));
    (void)vfs_write_file("/dev/input/usbkbd0", "type=input\nprotocol=usb-hid\nrole=keyboard\n", (uint32_t)dm_len("type=input\nprotocol=usb-hid\nrole=keyboard\n"));
    (void)vfs_write_file("/dev/bus/usb0", "type=bus\nprotocol=usb\nhotplug=yes\n", (uint32_t)dm_len("type=bus\nprotocol=usb\nhotplug=yes\n"));

    k_memset(fb_meta, 0, sizeof(fb_meta));
    dm_cat(fb_meta, "type=framebuffer\nwidth=", sizeof(fb_meta));
    {
        char nb[16];
        dm_u32(vbe.width, nb, sizeof(nb));
        dm_cat(fb_meta, nb, sizeof(fb_meta));
        dm_cat(fb_meta, "\nheight=", sizeof(fb_meta));
        dm_u32(vbe.height, nb, sizeof(nb));
        dm_cat(fb_meta, nb, sizeof(fb_meta));
        dm_cat(fb_meta, "\nbpp=32\n", sizeof(fb_meta));
    }
    (void)vfs_write_file("/dev/fb0", fb_meta, (uint32_t)dm_len(fb_meta));

    net_get_ip_str(net_eth0.ip, ip);
    net_get_mac_str(net_eth0.mac, mac);
    k_memset(net_meta, 0, sizeof(net_meta));
    dm_cat(net_meta, "type=net\niface=eth0\nip=", sizeof(net_meta));
    dm_cat(net_meta, ip, sizeof(net_meta));
    dm_cat(net_meta, "\nmac=", sizeof(net_meta));
    dm_cat(net_meta, mac, sizeof(net_meta));
    dm_cat(net_meta, "\ndhcp=on\n", sizeof(net_meta));
    (void)vfs_write_file("/dev/net/eth0", net_meta, (uint32_t)dm_len(net_meta));
    if (net_wlan0.wifi_enabled) {
        char wifi_meta[256];
        k_memset(wifi_meta, 0, sizeof(wifi_meta));
        dm_cat(wifi_meta, "type=net\niface=wlan0\nssid=", sizeof(wifi_meta));
        dm_cat(wifi_meta, net_wlan0.ssid[0] ? net_wlan0.ssid : "Nova-Lab", sizeof(wifi_meta));
        dm_cat(wifi_meta, "\nsignal=", sizeof(wifi_meta));
        {
            char nb[16];
            dm_u32((uint32_t)(net_wlan0.signal < 0 ? 0 : net_wlan0.signal), nb, sizeof(nb));
            dm_cat(wifi_meta, nb, sizeof(wifi_meta));
        }
        dm_cat(wifi_meta, "\n", sizeof(wifi_meta));
        (void)vfs_write_file("/dev/net/wlan0", wifi_meta, (uint32_t)dm_len(wifi_meta));
    }

    k_memset(audio_meta, 0, sizeof(audio_meta));
    dm_cat(audio_meta, "type=audio\nbackend=", sizeof(audio_meta));
    dm_cat(audio_meta, sound_driver_stack(), sizeof(audio_meta));
    dm_cat(audio_meta, "\n", sizeof(audio_meta));
    (void)vfs_write_file("/dev/snd/pcmC0D0p", audio_meta, (uint32_t)dm_len(audio_meta));
}

static void dm_build_devices_report(char *buf, int max) {
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    dm_line(buf, "NODE TYPE", max);
    dm_line(buf, "---------", max);
    dm_line(buf, "/dev/tty0 console", max);
    dm_line(buf, "/dev/null sink", max);
    dm_line(buf, "/dev/random pseudo-rng", max);
    dm_line(buf, "/dev/input/keyboard0 ps2-keyboard", max);
    dm_line(buf, "/dev/input/mouse0 ps2-mouse", max);
    dm_line(buf, "/dev/input/usbkbd0 usb-hid", max);
    dm_line(buf, "/dev/fb0 framebuffer", max);
    dm_line(buf, "/dev/net/eth0 network", max);
    dm_line(buf, "/dev/snd/pcmC0D0p audio", max);
    dm_line(buf, "/dev/bus/usb0 usb-host", max);
}

static void dm_publish_sysfs(void) {
    char drivers_report[4096];
    char pci_report[4096];
    char devices_report[1024];
    char manager_status[256];
    char driver_log[4096];
    char docs[4096];

    dm_ensure_dir("/sys");
    dm_ensure_dir("/sys/bus");
    dm_ensure_dir("/sys/bus/pci");
    dm_ensure_dir("/sys/bus/pci/devices");
    dm_ensure_dir("/sys/class");
    dm_ensure_dir("/sys/class/input");
    dm_ensure_dir("/sys/class/graphics");
    dm_ensure_dir("/sys/class/net");
    dm_ensure_dir("/sys/class/sound");
    dm_ensure_dir("/sys/kernel");
    dm_ensure_dir("/sys/kernel/drivers");
    dm_ensure_dir("/sys/drivers");
    dm_ensure_dir("/sys/firmware");
    dm_ensure_dir("/sys/firmware/pci");
    dm_ensure_dir("/var/log");

    driver_manager_report(drivers_report, sizeof(drivers_report));
    pci_describe_all(pci_report, sizeof(pci_report));
    dm_build_devices_report(devices_report, sizeof(devices_report));

    (void)vfs_write_file("/proc/drivers", drivers_report, (uint32_t)dm_len(drivers_report));
    (void)vfs_write_file("/proc/pci", pci_report, (uint32_t)dm_len(pci_report));
    (void)vfs_write_file("/proc/devices", devices_report, (uint32_t)dm_len(devices_report));
    (void)vfs_write_file("/sys/kernel/drivers/list", drivers_report, (uint32_t)dm_len(drivers_report));
    (void)vfs_write_file("/sys/firmware/pci/summary", pci_report, (uint32_t)dm_len(pci_report));
    (void)vfs_write_file("/sys/class/input/keyboard0", "/dev/input/keyboard0\n", (uint32_t)dm_len("/dev/input/keyboard0\n"));
    (void)vfs_write_file("/sys/class/input/mouse0", "/dev/input/mouse0\n", (uint32_t)dm_len("/dev/input/mouse0\n"));
    (void)vfs_write_file("/sys/class/graphics/fb0", "/dev/fb0\n", (uint32_t)dm_len("/dev/fb0\n"));
    (void)vfs_write_file("/sys/class/net/eth0", "/dev/net/eth0\n", (uint32_t)dm_len("/dev/net/eth0\n"));
    if (net_wlan0.wifi_enabled) (void)vfs_write_file("/sys/class/net/wlan0", "/dev/net/wlan0\n", (uint32_t)dm_len("/dev/net/wlan0\n"));
    (void)vfs_write_file("/sys/class/sound/card0", "/dev/snd/pcmC0D0p\n", (uint32_t)dm_len("/dev/snd/pcmC0D0p\n"));
    (void)vfs_write_file("/sys/drivers/list", drivers_report, (uint32_t)dm_len(drivers_report));

    k_memset(manager_status, 0, sizeof(manager_status));
    dm_cat(manager_status, "driver_count=", sizeof(manager_status));
    {
        char nb[16];
        dm_u32((uint32_t)g_driver_count, nb, sizeof(nb));
        dm_cat(manager_status, nb, sizeof(manager_status));
        dm_cat(manager_status, "\npci_devices=", sizeof(manager_status));
        dm_u32((uint32_t)pci_device_count(), nb, sizeof(nb));
        dm_cat(manager_status, nb, sizeof(manager_status));
        dm_cat(manager_status, "\nstatus=ready\n", sizeof(manager_status));
    }
    (void)vfs_write_file("/sys/kernel/drivers/manager", manager_status, (uint32_t)dm_len(manager_status));
    (void)vfs_write_file("/proc/driver_manager", manager_status, (uint32_t)dm_len(manager_status));

    k_memset(driver_log, 0, sizeof(driver_log));
    dm_line(driver_log, "[driver-manager] ready", sizeof(driver_log));
    dm_cat(driver_log, "[driver-manager] drivers=", sizeof(driver_log));
    {
        char nb[16];
        dm_u32((uint32_t)g_driver_count, nb, sizeof(nb));
        dm_cat(driver_log, nb, sizeof(driver_log));
        dm_cat(driver_log, "\n[driver-manager] pci_devices=", sizeof(driver_log));
        dm_u32((uint32_t)pci_device_count(), nb, sizeof(nb));
        dm_cat(driver_log, nb, sizeof(driver_log));
        dm_cat(driver_log, "\n", sizeof(driver_log));
    }
    dm_cat(driver_log, drivers_report, sizeof(driver_log));
    dm_cat(driver_log, "\n", sizeof(driver_log));
    dm_cat(driver_log, pci_report, sizeof(driver_log));
    (void)vfs_write_file("/var/log/driver-manager.log", driver_log, (uint32_t)dm_len(driver_log));
    (void)vfs_write_file("/var/log/pci-scan.log", pci_report, (uint32_t)dm_len(pci_report));

    for (int i = 0; i < pci_device_count(); ++i) {
        const nova_pci_device_t *dev = pci_get_device(i);
        char file_path[64];
        char meta[256];
        char b[3], s[3], f[3], ven[5], did[5], cls[3], sub[3], irq[3];
        if (!dev) continue;
        k_memset(file_path, 0, sizeof(file_path));
        dm_copy(file_path, "/sys/bus/pci/devices/", sizeof(file_path));
        dm_hex8(dev->bus, b); dm_hex8(dev->slot, s); dm_hex8(dev->function, f);
        dm_cat(file_path, b, sizeof(file_path));
        dm_cat(file_path, "_", sizeof(file_path));
        dm_cat(file_path, s, sizeof(file_path));
        dm_cat(file_path, "_", sizeof(file_path));
        dm_cat(file_path, f, sizeof(file_path));
        dm_cat(file_path, ".txt", sizeof(file_path));

        dm_hex16(dev->vendor_id, ven); dm_hex16(dev->device_id, did);
        dm_hex8(dev->class_code, cls); dm_hex8(dev->subclass, sub); dm_hex8(dev->irq_line, irq);
        k_memset(meta, 0, sizeof(meta));
        dm_cat(meta, "bdf=", sizeof(meta)); dm_cat(meta, b, sizeof(meta)); dm_cat(meta, ":", sizeof(meta)); dm_cat(meta, s, sizeof(meta)); dm_cat(meta, ".", sizeof(meta)); dm_cat(meta, f, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        dm_cat(meta, "vendor=0x", sizeof(meta)); dm_cat(meta, ven, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        dm_cat(meta, "device=0x", sizeof(meta)); dm_cat(meta, did, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        dm_cat(meta, "class=0x", sizeof(meta)); dm_cat(meta, cls, sizeof(meta)); dm_cat(meta, "\nsubclass=0x", sizeof(meta)); dm_cat(meta, sub, sizeof(meta)); dm_cat(meta, "\nirq=0x", sizeof(meta)); dm_cat(meta, irq, sizeof(meta)); dm_cat(meta, "\nbinding=", sizeof(meta)); dm_cat(meta, dev->binding, sizeof(meta)); dm_cat(meta, "\nlabel=", sizeof(meta)); dm_cat(meta, dev->label, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        (void)vfs_write_file(file_path, meta, (uint32_t)dm_len(meta));
    }

    for (int i = 0; i < g_driver_count; ++i) {
        const nova_driver_info_t *drv = &g_drivers[i];
        char path[80];
        char meta[256];
        char irq[16];
        k_memset(path, 0, sizeof(path));
        k_memset(meta, 0, sizeof(meta));
        dm_copy(path, "/sys/drivers/", sizeof(path));
        dm_cat(path, drv->name, sizeof(path));
        dm_cat(path, ".info", sizeof(path));
        dm_cat(meta, "name=", sizeof(meta)); dm_cat(meta, drv->name, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        dm_cat(meta, "bus=", sizeof(meta)); dm_cat(meta, dm_bus_name(drv->bus), sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        dm_cat(meta, "category=", sizeof(meta)); dm_cat(meta, drv->category, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        dm_cat(meta, "binding=", sizeof(meta)); dm_cat(meta, drv->binding, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        dm_cat(meta, "device=", sizeof(meta)); dm_cat(meta, drv->device_path, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        dm_cat(meta, "summary=", sizeof(meta)); dm_cat(meta, drv->summary, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        dm_cat(meta, "irq=", sizeof(meta)); dm_u32((uint32_t)drv->irq, irq, sizeof(irq)); dm_cat(meta, irq, sizeof(meta)); dm_cat(meta, "\n", sizeof(meta));
        (void)vfs_write_file(path, meta, (uint32_t)dm_len(meta));
    }

    k_memset(docs, 0, sizeof(docs));
    dm_line(docs, "Nova Driver Stack", sizeof(docs));
    dm_line(docs, "=================", sizeof(docs));
    dm_line(docs, "Elements ajoutes dans cette iteration :", sizeof(docs));
    dm_line(docs, "- API commune de pilotes et driver manager", sizeof(docs));
    dm_line(docs, "- Detection PCI automatique via ports 0xCF8/0xCFC", sizeof(docs));
    dm_line(docs, "- Publication /proc/drivers, /proc/pci, /proc/devices, /sys/* et /dev/*", sizeof(docs));
    dm_line(docs, "- Surfaces pour USB HID, framebuffer, reseau et audio", sizeof(docs));
    dm_line(docs, "", sizeof(docs));
    dm_cat(docs, drivers_report, sizeof(docs));
    (void)vfs_write_file("/home/user/Documents/Drivers.txt", docs, (uint32_t)dm_len(docs));
}

void driver_manager_init(void) {
    if (g_driver_manager_ready) return;
    k_memset(g_drivers, 0, sizeof(g_drivers));
    g_driver_count = 0;
    pci_init();
    dm_register_core_drivers();
    dm_register_pci_backed_drivers();
    dm_publish_device_nodes();
    dm_publish_sysfs();
    g_driver_manager_ready = 1;
}

int driver_manager_count(void) {
    return g_driver_count;
}

const nova_driver_info_t* driver_manager_get(int index) {
    if (index < 0 || index >= g_driver_count) return NULL;
    return &g_drivers[index];
}

void driver_manager_report(char *buf, int max) {
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    dm_cat(buf, "NAME BUS CAT IRQ BIND DEVICE SUMMARY\n", max);
    dm_cat(buf, "------------------------------------\n", max);
    for (int i = 0; i < g_driver_count; ++i) {
        char line[256];
        char irq[16];
        char ven[5];
        char did[5];
        const nova_driver_info_t *drv = &g_drivers[i];
        k_memset(line, 0, sizeof(line));
        dm_u32((uint32_t)drv->irq, irq, sizeof(irq));
        dm_hex16(drv->vendor_id, ven);
        dm_hex16(drv->device_id, did);
        dm_cat(line, drv->name, sizeof(line));
        dm_cat(line, " bus=", sizeof(line));
        dm_cat(line, dm_bus_name(drv->bus), sizeof(line));
        dm_cat(line, " cat=", sizeof(line));
        dm_cat(line, drv->category, sizeof(line));
        dm_cat(line, " irq=", sizeof(line));
        dm_cat(line, irq, sizeof(line));
        dm_cat(line, " bind=", sizeof(line));
        dm_cat(line, drv->binding, sizeof(line));
        dm_cat(line, " dev=", sizeof(line));
        dm_cat(line, drv->device_path, sizeof(line));
        if (drv->vendor_id || drv->device_id) {
            dm_cat(line, " pci=", sizeof(line));
            dm_cat(line, ven, sizeof(line));
            dm_cat(line, ":", sizeof(line));
            dm_cat(line, did, sizeof(line));
        }
        dm_cat(line, " ", sizeof(line));
        dm_cat(line, drv->summary, sizeof(line));
        dm_cat(line, "\n", sizeof(line));
        dm_cat(buf, line, max);
    }
}
