#ifndef NOVA_NET_H
#define NOVA_NET_H

#include <stdint.h>
#include "stack.h"

typedef struct {
    char     iface[16];
    uint8_t  mac[6];
    uint8_t  ip[4];
    uint8_t  gateway[4];
    uint8_t  netmask[4];
    uint8_t  dns[4];
    uint8_t  ipv6[16];
    int      connected;
    int      dhcp_enabled;
    int      ipv6_enabled;
    int      firewall_enabled;
    int      vpn_enabled;
    int      bridge_enabled;
    int      nat_enabled;
    int      wol_enabled;
    int      qos_enabled;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_packets;
    uint32_t tx_packets;
    char     ssid[64];
    char     resolver[32];
    char     vpn_profile[32];
    char     qos_profile[32];
    int      signal;
    int      wifi_enabled;
} net_iface_t;

extern net_iface_t net_eth0;
extern net_iface_t net_wlan0;

void net_init(void);
void net_get_ip_str(uint8_t *ip, char *buf);
void net_get_mac_str(uint8_t *mac, char *buf);
void net_get_ipv6_str(uint8_t *ipv6, char *buf, int max);
void net_feature_report(char *buf, int max);
int  net_ping(const char *host);

#endif
