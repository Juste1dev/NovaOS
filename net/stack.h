#ifndef NOVA_NET_STACK_H
#define NOVA_NET_STACK_H

#include <stdint.h>

#define NOVA_ARP_TABLE_MAX   8
#define NOVA_DNS_CACHE_MAX   8
#define NOVA_TCP_SOCKET_MAX  8
#define NOVA_UDP_SOCKET_MAX  8

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    uint32_t age_ms;
    int valid;
} nova_arp_entry_t;

typedef struct {
    char host[64];
    uint8_t ip[4];
    uint32_t ttl_seconds;
    int valid;
} nova_dns_entry_t;

typedef struct {
    uint8_t offered_ip[4];
    uint8_t server_ip[4];
    uint8_t router[4];
    uint8_t dns[4];
    uint32_t lease_seconds;
    uint32_t renew_after_seconds;
    int bound;
} nova_dhcp_lease_t;

typedef struct {
    int id;
    char remote_host[64];
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t seq;
    uint32_t ack;
    int established;
} nova_tcp_socket_t;

typedef struct {
    int id;
    uint16_t local_port;
    uint16_t remote_port;
    char remote_host[64];
    int bound;
} nova_udp_socket_t;

void net_stack_init(void);
void net_stack_report(char *buf, int max);
void net_stack_publish_procfs(void);
int  net_dns_resolve(const char *host, uint8_t out_ip[4]);
int  net_dhcp_renew(void);
int  net_tcp_connect(const char *host, uint16_t port);
int  net_udp_bind(uint16_t port);

#endif
