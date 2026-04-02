#ifndef _KERNEL_RTL8139_H
#define _KERNEL_RTL8139_H

#include <stdint.h>

typedef struct rtl8139_stats {
    uint32_t interrupts;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_irqs;
    uint32_t tx_irqs;
    uint32_t link_events;
    uint32_t rx_drops;
} rtl8139_stats_t;

typedef struct rtl8139_debug_info {
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t io_base;
    uint8_t irq;
    uint32_t command;
    uint32_t media_status;
} rtl8139_debug_info_t;

int rtl8139_initialize(void);
int rtl8139_is_ready(void);
int rtl8139_link_up(void);
int rtl8139_send_raw(const void* data, uint16_t len);
int rtl8139_send_test_frame(void);
void rtl8139_get_mac(uint8_t out_mac[6]);
void rtl8139_get_stats(rtl8139_stats_t* out_stats);
void rtl8139_get_debug_info(rtl8139_debug_info_t* out_info);

#endif

