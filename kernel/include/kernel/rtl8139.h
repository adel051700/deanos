#ifndef _KERNEL_RTL8139_H
#define _KERNEL_RTL8139_H

#include <stdint.h>

typedef void (*rtl8139_rx_callback_t)(const uint8_t* frame, uint16_t len);

typedef struct rtl8139_stats {
    uint32_t interrupts;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_irqs;
    uint32_t tx_irqs;
    uint32_t link_events;
    uint32_t rx_drops;
} rtl8139_stats_t;

int rtl8139_initialize(void);
int rtl8139_is_ready(void);
int rtl8139_link_up(void);
int rtl8139_send_raw(const void* data, uint16_t len);
void rtl8139_set_rx_callback(rtl8139_rx_callback_t cb);
void rtl8139_get_mac(uint8_t out_mac[6]);

#endif

