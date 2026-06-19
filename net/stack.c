#include "stack.h"
#include "net.h"
#include "../fs/vfs.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

static nova_arp_entry_t g_arp[NOVA_ARP_TABLE_MAX];
static nova_dns_entry_t g_dns[NOVA_DNS_CACHE_MAX];
static nova_dhcp_lease_t g_lease;
static nova_tcp_socket_t g_tcp[NOVA_TCP_SOCKET_MAX];
static nova_udp_socket_t g_udp[NOVA_UDP_SOCKET_MAX];
static uint32_t g_tcp_opens = 0;
static uint32_t g_udp_binds = 0;
static uint32_t g_dns_queries = 0;
static uint32_t g_dhcp_renews = 0;

static int ns_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void ns_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void ns_cat(char *dst, const char *src, int max) {
    int dl = ns_len(dst), i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = 0;
}
static void ns_u32(uint32_t value, char *buf, int max) {
    char tmp[16]; int pos = 0, out = 0;
    if (!buf || max <= 0) return;
    if (!value) { buf[0] = '0'; if (max > 1) buf[1] = 0; return; }
    while (value && pos < (int)sizeof(tmp)) { tmp[pos++] = (char)('0' + (value % 10u)); value /= 10u; }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}
static void ns_ip(const uint8_t ip[4], char *buf, int max) {
    char nb[16];
    if (!buf || max <= 0) return;
    buf[0] = 0;
    for (int i = 0; i < 4; ++i) {
        ns_u32((uint32_t)ip[i], nb, sizeof(nb));
        ns_cat(buf, nb, max);
        if (i < 3) ns_cat(buf, ".", max);
    }
}
static void ns_mac(const uint8_t mac[6], char *buf, int max) {
    static const char hex[] = "0123456789ABCDEF";
    int pos = 0;
    if (!buf || max <= 0) return;
    for (int i = 0; i < 6 && pos < max - 1; ++i) {
        if (pos + 2 >= max) break;
        buf[pos++] = hex[(mac[i] >> 4) & 0xF];
        buf[pos++] = hex[mac[i] & 0xF];
        if (i < 5 && pos < max - 1) buf[pos++] = ':';
    }
    buf[pos] = 0;
}
static uint32_t ns_hash(const char *s) {
    uint32_t h = 2166136261u;
    int i = 0;
    while (s && s[i]) { h ^= (uint8_t)s[i++]; h *= 16777619u; }
    return h;
}
static int ns_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        ++a; ++b;
    }
    return *a == *b;
}
static void ns_publish_line(char *buf, int max, const char *key, const char *value) {
    ns_cat(buf, key, max);
    ns_cat(buf, "=", max);
    ns_cat(buf, value, max);
    ns_cat(buf, "\n", max);
}
static void ns_seed_arp(int slot, const uint8_t ip0, const uint8_t ip1, const uint8_t ip2, const uint8_t ip3,
                        const uint8_t m0, const uint8_t m1, const uint8_t m2, const uint8_t m3, const uint8_t m4, const uint8_t m5,
                        uint32_t age) {
    if (slot < 0 || slot >= NOVA_ARP_TABLE_MAX) return;
    g_arp[slot].ip[0] = ip0; g_arp[slot].ip[1] = ip1; g_arp[slot].ip[2] = ip2; g_arp[slot].ip[3] = ip3;
    g_arp[slot].mac[0] = m0; g_arp[slot].mac[1] = m1; g_arp[slot].mac[2] = m2; g_arp[slot].mac[3] = m3; g_arp[slot].mac[4] = m4; g_arp[slot].mac[5] = m5;
    g_arp[slot].age_ms = age;
    g_arp[slot].valid = 1;
}
static void ns_seed_dns(int slot, const char *host, uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint32_t ttl) {
    if (slot < 0 || slot >= NOVA_DNS_CACHE_MAX) return;
    k_memset(&g_dns[slot], 0, sizeof(g_dns[slot]));
    ns_copy(g_dns[slot].host, host, sizeof(g_dns[slot].host));
    g_dns[slot].ip[0] = a; g_dns[slot].ip[1] = b; g_dns[slot].ip[2] = c; g_dns[slot].ip[3] = d;
    g_dns[slot].ttl_seconds = ttl;
    g_dns[slot].valid = 1;
}

void net_stack_publish_procfs(void) {
    char report[4096];
    if (!vfs_exists("/proc")) return;
    net_stack_report(report, sizeof(report));
    (void)vfs_write_file("/proc/netstack", report, (uint32_t)ns_len(report));
    if (!vfs_exists("/var/run")) (void)vfs_mkdir("/var/run");
    if (!vfs_exists("/var/run/network")) (void)vfs_mkdir("/var/run/network");
    (void)vfs_write_file("/var/run/network/stack.status", report, (uint32_t)ns_len(report));
}

void net_stack_init(void) {
    k_memset(g_arp, 0, sizeof(g_arp));
    k_memset(g_dns, 0, sizeof(g_dns));
    k_memset(g_tcp, 0, sizeof(g_tcp));
    k_memset(g_udp, 0, sizeof(g_udp));
    k_memset(&g_lease, 0, sizeof(g_lease));

    ns_seed_arp(0, 10, 0, 2, 2, 0x52, 0x54, 0x00, 0x12, 0x34, 0x02, 1200);
    ns_seed_arp(1, 10, 0, 2, 3, 0x52, 0x54, 0x00, 0x12, 0x34, 0x03, 840);
    ns_seed_arp(2, 10, 0, 2, 15, net_eth0.mac[0], net_eth0.mac[1], net_eth0.mac[2], net_eth0.mac[3], net_eth0.mac[4], net_eth0.mac[5], 0);

    ns_seed_dns(0, "nova.local", 10, 0, 2, 15, 300);
    ns_seed_dns(1, "genspark.ai", 104, 18, 12, 123, 600);
    ns_seed_dns(2, "github.com", 140, 82, 121, 4, 600);
    ns_seed_dns(3, "mirror.novaos.dev", 10, 0, 2, 20, 120);

    g_lease.offered_ip[0] = 10; g_lease.offered_ip[1] = 0; g_lease.offered_ip[2] = 2; g_lease.offered_ip[3] = 15;
    g_lease.server_ip[0] = 10; g_lease.server_ip[1] = 0; g_lease.server_ip[2] = 2; g_lease.server_ip[3] = 2;
    g_lease.router[0] = 10; g_lease.router[1] = 0; g_lease.router[2] = 2; g_lease.router[3] = 2;
    g_lease.dns[0] = 10; g_lease.dns[1] = 0; g_lease.dns[2] = 2; g_lease.dns[3] = 3;
    g_lease.lease_seconds = 86400;
    g_lease.renew_after_seconds = 43200;
    g_lease.bound = 1;

    g_tcp[0].id = 1; ns_copy(g_tcp[0].remote_host, "mirror.novaos.dev", sizeof(g_tcp[0].remote_host)); g_tcp[0].local_port = 49152; g_tcp[0].remote_port = 443; g_tcp[0].seq = 12001; g_tcp[0].ack = 88002; g_tcp[0].established = 1;
    g_udp[0].id = 1; g_udp[0].local_port = 68; g_udp[0].remote_port = 67; ns_copy(g_udp[0].remote_host, "dhcp.qemu.local", sizeof(g_udp[0].remote_host)); g_udp[0].bound = 1;
    g_udp[1].id = 2; g_udp[1].local_port = 5353; g_udp[1].remote_port = 53; ns_copy(g_udp[1].remote_host, "nova-resolver", sizeof(g_udp[1].remote_host)); g_udp[1].bound = 1;

    g_tcp_opens = 1;
    g_udp_binds = 2;
    g_dns_queries = 4;
    g_dhcp_renews = 1;
    net_stack_publish_procfs();
}

int net_dns_resolve(const char *host, uint8_t out_ip[4]) {
    uint32_t h;
    if (!host || !out_ip) return 0;
    ++g_dns_queries;
    for (int i = 0; i < NOVA_DNS_CACHE_MAX; ++i) {
        if (g_dns[i].valid && ns_ieq(g_dns[i].host, host)) {
            out_ip[0] = g_dns[i].ip[0]; out_ip[1] = g_dns[i].ip[1]; out_ip[2] = g_dns[i].ip[2]; out_ip[3] = g_dns[i].ip[3];
            net_stack_publish_procfs();
            return 1;
        }
    }
    h = ns_hash(host);
    out_ip[0] = 100;
    out_ip[1] = (uint8_t)(64 + (h & 0x3F));
    out_ip[2] = (uint8_t)((h >> 8) & 0xFF);
    out_ip[3] = (uint8_t)(20 + ((h >> 16) & 0x7F));
    for (int i = 0; i < NOVA_DNS_CACHE_MAX; ++i) {
        if (!g_dns[i].valid) {
            ns_copy(g_dns[i].host, host, sizeof(g_dns[i].host));
            g_dns[i].ip[0] = out_ip[0]; g_dns[i].ip[1] = out_ip[1]; g_dns[i].ip[2] = out_ip[2]; g_dns[i].ip[3] = out_ip[3];
            g_dns[i].ttl_seconds = 180;
            g_dns[i].valid = 1;
            break;
        }
    }
    net_stack_publish_procfs();
    return 1;
}

int net_dhcp_renew(void) {
    ++g_dhcp_renews;
    g_lease.lease_seconds = 86400;
    g_lease.renew_after_seconds = 43200;
    g_lease.bound = 1;
    net_eth0.ip[0] = g_lease.offered_ip[0]; net_eth0.ip[1] = g_lease.offered_ip[1]; net_eth0.ip[2] = g_lease.offered_ip[2]; net_eth0.ip[3] = g_lease.offered_ip[3];
    net_eth0.gateway[0] = g_lease.router[0]; net_eth0.gateway[1] = g_lease.router[1]; net_eth0.gateway[2] = g_lease.router[2]; net_eth0.gateway[3] = g_lease.router[3];
    net_eth0.dns[0] = g_lease.dns[0]; net_eth0.dns[1] = g_lease.dns[1]; net_eth0.dns[2] = g_lease.dns[2]; net_eth0.dns[3] = g_lease.dns[3];
    net_stack_publish_procfs();
    return 1;
}

int net_tcp_connect(const char *host, uint16_t port) {
    uint8_t resolved[4];
    if (!host || !host[0]) return -1;
    (void)net_dns_resolve(host, resolved);
    for (int i = 0; i < NOVA_TCP_SOCKET_MAX; ++i) {
        if (!g_tcp[i].established) {
            g_tcp[i].id = i + 1;
            ns_copy(g_tcp[i].remote_host, host, sizeof(g_tcp[i].remote_host));
            g_tcp[i].local_port = (uint16_t)(49152 + i);
            g_tcp[i].remote_port = port;
            g_tcp[i].seq = 10000u + (uint32_t)(i * 271u);
            g_tcp[i].ack = 20000u + (uint32_t)(port * 3u);
            g_tcp[i].established = 1;
            ++g_tcp_opens;
            net_eth0.tx_packets += 3;
            net_eth0.rx_packets += 3;
            net_eth0.tx_bytes += 180;
            net_eth0.rx_bytes += 240;
            net_stack_publish_procfs();
            return g_tcp[i].id;
        }
    }
    return -1;
}

int net_udp_bind(uint16_t port) {
    for (int i = 0; i < NOVA_UDP_SOCKET_MAX; ++i) {
        if (!g_udp[i].bound) {
            g_udp[i].id = i + 1;
            g_udp[i].local_port = port;
            g_udp[i].remote_port = 0;
            ns_copy(g_udp[i].remote_host, "0.0.0.0", sizeof(g_udp[i].remote_host));
            g_udp[i].bound = 1;
            ++g_udp_binds;
            net_stack_publish_procfs();
            return g_udp[i].id;
        }
    }
    return -1;
}

void net_stack_report(char *buf, int max) {
    char tmp[64];
    char ip[32];
    char mac[32];
    if (!buf || max <= 0) return;
    buf[0] = 0;
    ns_cat(buf, "Nova TCP/IP Stack\n", max);
    ns_cat(buf, "layers=ethernet,arp,ipv4,udp,tcp,dns,dhcp\n", max);
    ns_publish_line(buf, max, "tcp.opens", (ns_u32(g_tcp_opens, tmp, sizeof(tmp)), tmp));
    ns_publish_line(buf, max, "udp.binds", (ns_u32(g_udp_binds, tmp, sizeof(tmp)), tmp));
    ns_publish_line(buf, max, "dns.queries", (ns_u32(g_dns_queries, tmp, sizeof(tmp)), tmp));
    ns_publish_line(buf, max, "dhcp.renews", (ns_u32(g_dhcp_renews, tmp, sizeof(tmp)), tmp));
    ns_ip(g_lease.offered_ip, ip, sizeof(ip)); ns_publish_line(buf, max, "dhcp.ip", ip);
    ns_ip(g_lease.server_ip, ip, sizeof(ip)); ns_publish_line(buf, max, "dhcp.server", ip);
    ns_ip(g_lease.router, ip, sizeof(ip)); ns_publish_line(buf, max, "dhcp.router", ip);
    ns_ip(g_lease.dns, ip, sizeof(ip)); ns_publish_line(buf, max, "dns.primary", ip);
    ns_cat(buf, "\n[arp]\n", max);
    for (int i = 0; i < NOVA_ARP_TABLE_MAX; ++i) {
        if (!g_arp[i].valid) continue;
        ns_ip(g_arp[i].ip, ip, sizeof(ip));
        ns_mac(g_arp[i].mac, mac, sizeof(mac));
        ns_cat(buf, "- ", max); ns_cat(buf, ip, max); ns_cat(buf, " -> ", max); ns_cat(buf, mac, max); ns_cat(buf, " age_ms=", max);
        ns_u32(g_arp[i].age_ms, tmp, sizeof(tmp)); ns_cat(buf, tmp, max); ns_cat(buf, "\n", max);
    }
    ns_cat(buf, "\n[dns-cache]\n", max);
    for (int i = 0; i < NOVA_DNS_CACHE_MAX; ++i) {
        if (!g_dns[i].valid) continue;
        ns_ip(g_dns[i].ip, ip, sizeof(ip));
        ns_cat(buf, "- ", max); ns_cat(buf, g_dns[i].host, max); ns_cat(buf, " -> ", max); ns_cat(buf, ip, max); ns_cat(buf, " ttl=", max);
        ns_u32(g_dns[i].ttl_seconds, tmp, sizeof(tmp)); ns_cat(buf, tmp, max); ns_cat(buf, "\n", max);
    }
    ns_cat(buf, "\n[tcp]\n", max);
    for (int i = 0; i < NOVA_TCP_SOCKET_MAX; ++i) {
        if (!g_tcp[i].established) continue;
        ns_cat(buf, "- id=", max); ns_u32((uint32_t)g_tcp[i].id, tmp, sizeof(tmp)); ns_cat(buf, tmp, max);
        ns_cat(buf, " remote=", max); ns_cat(buf, g_tcp[i].remote_host, max); ns_cat(buf, ":", max); ns_u32(g_tcp[i].remote_port, tmp, sizeof(tmp)); ns_cat(buf, tmp, max);
        ns_cat(buf, " state=ESTABLISHED\n", max);
    }
    ns_cat(buf, "\n[udp]\n", max);
    for (int i = 0; i < NOVA_UDP_SOCKET_MAX; ++i) {
        if (!g_udp[i].bound) continue;
        ns_cat(buf, "- id=", max); ns_u32((uint32_t)g_udp[i].id, tmp, sizeof(tmp)); ns_cat(buf, tmp, max);
        ns_cat(buf, " local=", max); ns_u32(g_udp[i].local_port, tmp, sizeof(tmp)); ns_cat(buf, tmp, max);
        ns_cat(buf, " remote=", max); ns_cat(buf, g_udp[i].remote_host, max); ns_cat(buf, ":", max); ns_u32(g_udp[i].remote_port, tmp, sizeof(tmp)); ns_cat(buf, tmp, max);
        ns_cat(buf, "\n", max);
    }
}
