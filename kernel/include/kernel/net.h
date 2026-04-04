#ifndef _KERNEL_NET_H
#define _KERNEL_NET_H

#include <stdint.h>

typedef struct net_stats {
    uint32_t interrupts;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_irqs;
    uint32_t tx_irqs;
    uint32_t link_events;
    uint32_t rx_drops;
    uint32_t arp_rx;
    uint32_t arp_tx;
    uint32_t ipv4_rx;
    uint32_t ipv4_tx;
    uint32_t icmp_rx;
    uint32_t icmp_tx;
    uint32_t ping_requests;
    uint32_t ping_replies;
} net_stats_t;

typedef struct net_ipv4_config {
    uint8_t address[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t configured;
} net_ipv4_config_t;

typedef struct net_ping_result {
    uint32_t elapsed_ms;
    uint16_t sequence;
    uint8_t ttl;
    uint8_t received;
} net_ping_result_t;

typedef struct net_debug_info {
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t io_base;
    uint8_t irq;
    uint32_t reg_a;
    uint32_t reg_b;
    uint32_t reg_c;
    uint32_t reg_d;
} net_debug_info_t;

int net_initialize(void);
int net_is_ready(void);
int net_link_up(void);
const char* net_driver_name(void);
int net_send_test_frame(void);
int net_set_ipv4_config(const net_ipv4_config_t* config);
int net_get_ipv4_config(net_ipv4_config_t* out_config);
int net_ping(const uint8_t target_ip[4], uint32_t timeout_ms, net_ping_result_t* out_result);
void net_on_receive(const void* frame, uint16_t len);
void net_get_mac(uint8_t out_mac[6]);
void net_get_stats(net_stats_t* out_stats);
void net_get_debug_info(net_debug_info_t* out_info);

#endif

