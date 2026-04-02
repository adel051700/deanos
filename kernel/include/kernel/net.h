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
} net_stats_t;

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
void net_get_mac(uint8_t out_mac[6]);
void net_get_stats(net_stats_t* out_stats);
void net_get_debug_info(net_debug_info_t* out_info);

#endif

