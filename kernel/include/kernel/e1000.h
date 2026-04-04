#ifndef _KERNEL_E1000_H
#define _KERNEL_E1000_H

#include <stdint.h>

typedef void (*e1000_rx_callback_t)(const void* frame, uint16_t len);

typedef struct e1000_stats {
    uint32_t interrupts;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_irqs;
    uint32_t tx_irqs;
    uint32_t link_events;
    uint32_t rx_drops;
} e1000_stats_t;

typedef struct e1000_debug_info {
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t io_base;
    uint8_t irq;
    uint32_t ctrl;
    uint32_t status;
    uint32_t ral0;
    uint32_t rah0;
} e1000_debug_info_t;

int e1000_initialize(void);
int e1000_is_ready(void);
int e1000_link_up(void);
void e1000_set_rx_callback(e1000_rx_callback_t callback);
int e1000_send_raw(const void* data, uint16_t len);
int e1000_send_test_frame(void);
void e1000_get_mac(uint8_t out_mac[6]);
void e1000_get_stats(e1000_stats_t* out_stats);
void e1000_get_debug_info(e1000_debug_info_t* out_info);

#endif

