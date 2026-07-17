#ifndef _KERNEL_E1000_H
#define _KERNEL_E1000_H

#include <stdint.h>

typedef void (*e1000_rx_callback_t)(const uint8_t* frame, uint16_t len);

typedef struct e1000_stats {
    uint32_t interrupts;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_irqs;
    uint32_t tx_irqs;
    uint32_t link_events;
    uint32_t rx_drops;
} e1000_stats_t;

int e1000_initialize(void);
int e1000_is_ready(void);
int e1000_link_up(void);
int e1000_send_raw(const void* data, uint16_t len);
void e1000_set_rx_callback(e1000_rx_callback_t cb);
void e1000_get_mac(uint8_t out_mac[6]);

#endif

