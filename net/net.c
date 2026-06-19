#include "net.h"
#include "../fs/vfs.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

net_iface_t net_eth0;
net_iface_t net_wlan0;

static void byte_to_hex(uint8_t b, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = hex[(b >> 4) & 0xF];
    out[1] = hex[b & 0xF];
    out[2] = 0;
}

static void word_to_hex4(uint16_t value, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = hex[(value >> 12) & 0xF];
    out[1] = hex[(value >> 8) & 0xF];
    out[2] = hex[(value >> 4) & 0xF];
    out[3] = hex[value & 0xF];
    out[4] = 0;
}

static int net_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void net_strcpy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void net_strcat_n(char *dst, const char *src, int max) {
    int dl = net_strlen(dst), i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = 0;
}
static void net_extract_host(const char *input, char *host, int max) {
    int i = 0;
    if (!host || max <= 0) return;
    host[0] = 0;
    if (!input || !input[0]) { net_strcpy(host, "nova.local", max); return; }
    const char *p = input;
    const char *scheme = k_strstr(input, "://");
    if (scheme) p = scheme + 3;
    else if (k_strncmp(input, "about:", 6) == 0 || k_strncmp(input, "nova:", 5) == 0) { net_strcpy(host, "nova.local", max); return; }
    while (*p == '/') p++;
    while (p[i] && p[i] != '/' && p[i] != ':' && p[i] != '?' && p[i] != '#' && i < max - 1) { host[i] = p[i]; i++; }
    host[i] = 0;
    if (!host[0]) net_strcpy(host, "nova.local", max);
}
static uint32_t net_host_hash(const char *host) {
    uint32_t h = 5381u;
    int i = 0;
    while (host && host[i]) { h = ((h << 5) + h) ^ (uint8_t)host[i]; i++; }
    return h;
}
static int net_host_bonus(const char *host) {
    if (!host || !host[0]) return 4;
    if (k_strcmp(host, "nova.local") == 0) return 2;
    if (k_strcmp(host, "localhost") == 0) return 3;
    if (k_strcmp(host, "genspark.ai") == 0) return 18;
    if (k_strcmp(host, "github.com") == 0) return 22;
    if (k_strcmp(host, "wikipedia.org") == 0) return 26;
    if (k_strcmp(host, "news.ycombinator.com") == 0) return 28;
    return 30 + (int)(net_host_hash(host) % 45u);
}
static void net_publish_status(void) {
    char report[4096];
    if (!vfs_exists("/proc")) return;
    net_feature_report(report, sizeof(report));
    (void)vfs_write_file("/proc/network", report, (uint32_t)net_strlen(report));
}

void net_get_ip_str(uint8_t *ip, char *buf) {
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        int v = ip[i];
        if (v >= 100) { buf[pos++] = '0' + v/100; v %= 100; }
        if (ip[i] >= 10) { buf[pos++] = '0' + v/10; v %= 10; }
        buf[pos++] = '0' + v;
        if (i < 3) buf[pos++] = '.';
    }
    buf[pos] = 0;
}

void net_get_mac_str(uint8_t *mac, char *buf) {
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        byte_to_hex(mac[i], buf + pos);
        pos += 2;
        if (i < 5) buf[pos++] = ':';
    }
    buf[pos] = 0;
}

void net_get_ipv6_str(uint8_t *ipv6, char *buf, int max) {
    int pos = 0;
    if (!buf || max <= 0) return;
    buf[0] = 0;
    for (int i = 0; i < 8 && pos < max - 1; i++) {
        uint16_t word = (uint16_t)(((uint16_t)ipv6[i * 2] << 8) | ipv6[i * 2 + 1]);
        char tmp[5];
        word_to_hex4(word, tmp);
        for (int j = 0; j < 4 && pos < max - 1; j++) buf[pos++] = tmp[j];
        if (i < 7 && pos < max - 1) buf[pos++] = ':';
    }
    buf[pos] = 0;
}

void net_feature_report(char *buf, int max) {
    char ip[20], ipv6[64], gw[20], dns[20], mac[20], stack[3072];
    if (!buf || max <= 0) return;
    buf[0] = 0;
    net_get_ip_str(net_eth0.ip, ip);
    net_get_ipv6_str(net_eth0.ipv6, ipv6, sizeof(ipv6));
    net_get_ip_str(net_eth0.gateway, gw);
    net_get_ip_str(net_eth0.dns, dns);
    net_get_mac_str(net_eth0.mac, mac);
    net_strcpy(buf, "Reseau Nova\n", max);
    net_strcat_n(buf, "- Carte : RTL8139 / ", max); net_strcat_n(buf, net_eth0.iface, max); net_strcat_n(buf, "\n", max);
    net_strcat_n(buf, "- Stack TCP/IP : Ethernet + ARP + IPv4 + UDP + TCP\n", max);
    net_strcat_n(buf, "- Services : DNS resolver + DHCP client + tables procfs\n", max);
    net_strcat_n(buf, "- IPv4 : ", max); net_strcat_n(buf, ip, max); net_strcat_n(buf, "\n", max);
    net_strcat_n(buf, "- Gateway : ", max); net_strcat_n(buf, gw, max); net_strcat_n(buf, "\n", max);
    net_strcat_n(buf, "- DNS : ", max); net_strcat_n(buf, dns, max); net_strcat_n(buf, "\n", max);
    net_strcat_n(buf, "- MAC : ", max); net_strcat_n(buf, mac, max); net_strcat_n(buf, "\n", max);
    net_strcat_n(buf, "- IPv6 : ", max); net_strcat_n(buf, ipv6, max); net_strcat_n(buf, "\n\n", max);
    k_memset(stack, 0, sizeof(stack));
    net_stack_report(stack, sizeof(stack));
    net_strcat_n(buf, stack, max);
}

void net_init(void) {
    __builtin_memset(&net_eth0, 0, sizeof(net_eth0));
    __builtin_memset(&net_wlan0, 0, sizeof(net_wlan0));

    __builtin_memcpy(net_eth0.iface, "eth0", 5);
    net_eth0.mac[0] = 0x52; net_eth0.mac[1] = 0x54; net_eth0.mac[2] = 0x00; net_eth0.mac[3] = 0x12; net_eth0.mac[4] = 0x34; net_eth0.mac[5] = 0x56;
    net_eth0.ip[0] = 10; net_eth0.ip[1] = 0; net_eth0.ip[2] = 2; net_eth0.ip[3] = 15;
    net_eth0.gateway[0] = 10; net_eth0.gateway[1] = 0; net_eth0.gateway[2] = 2; net_eth0.gateway[3] = 2;
    net_eth0.netmask[0] = 255; net_eth0.netmask[1] = 255; net_eth0.netmask[2] = 255; net_eth0.netmask[3] = 0;
    net_eth0.dns[0] = 10; net_eth0.dns[1] = 0; net_eth0.dns[2] = 2; net_eth0.dns[3] = 3;
    net_eth0.ipv6[0] = 0x20; net_eth0.ipv6[1] = 0x01; net_eth0.ipv6[2] = 0x0d; net_eth0.ipv6[3] = 0xb8; net_eth0.ipv6[15] = 0x15;
    net_eth0.connected = 1; net_eth0.dhcp_enabled = 1; net_eth0.ipv6_enabled = 1; net_eth0.firewall_enabled = 1; net_eth0.vpn_enabled = 1; net_eth0.bridge_enabled = 1; net_eth0.nat_enabled = 1; net_eth0.wol_enabled = 1; net_eth0.qos_enabled = 1;
    __builtin_memcpy(net_eth0.resolver, "nova-resolver", 14);
    __builtin_memcpy(net_eth0.vpn_profile, "nova-lab-mesh", 14);
    __builtin_memcpy(net_eth0.qos_profile, "interactive-desktop", 20);
    net_eth0.rx_bytes = 8192; net_eth0.tx_bytes = 2048; net_eth0.rx_packets = 12; net_eth0.tx_packets = 6;

    __builtin_memcpy(net_wlan0.iface, "wlan0", 6);
    net_wlan0.mac[0] = 0xDA; net_wlan0.mac[1] = 0x7A; net_wlan0.mac[2] = 0x42; net_wlan0.mac[3] = 0xC0; net_wlan0.mac[4] = 0xFF; net_wlan0.mac[5] = 0xEE;
    __builtin_memcpy(net_wlan0.ssid, "Nova-Lab", 9);
    __builtin_memcpy(net_wlan0.resolver, "nova-resolver", 14);
    __builtin_memcpy(net_wlan0.vpn_profile, "standby", 8);
    __builtin_memcpy(net_wlan0.qos_profile, "wifi-balanced", 14);
    net_wlan0.signal = 72; net_wlan0.wifi_enabled = 1; net_wlan0.connected = 0; net_wlan0.ipv6_enabled = 1; net_wlan0.firewall_enabled = 1; net_wlan0.wol_enabled = 1; net_wlan0.qos_enabled = 1;

    net_stack_init();
    net_publish_status();
}

int net_ping(const char *host) {
    char resolved[96];
    uint8_t ip[4];
    int latency;
    if (!net_eth0.connected) return -1;
    net_extract_host(host, resolved, 96);
    latency = net_host_bonus(resolved);
    (void)net_dns_resolve(resolved, ip);
    net_eth0.tx_packets += 1;
    net_eth0.rx_packets += 1;
    net_eth0.tx_bytes += (uint32_t)(64 + net_strlen(resolved) * 2);
    net_eth0.rx_bytes += (uint32_t)(96 + net_strlen(resolved) * 3 + latency);
    if (k_strcmp(resolved, "offline.local") == 0) return -1;
    net_publish_status();
    return latency;
}
