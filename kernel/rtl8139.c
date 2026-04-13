#include "include/kernel/rtl8139.h"

#include "include/kernel/interrupt.h"
#include "include/kernel/io.h"
#include "include/kernel/irq.h"
#include "include/kernel/log.h"
#include "include/kernel/pci.h"

#include <string.h>

#define RTL8139_VENDOR_ID 0x10ECu
#define RTL8139_DEVICE_ID 0x8139u

#define RTL_REG_IDR0     0x00u
#define RTL_REG_TSD0     0x10u
#define RTL_REG_TSAD0    0x20u
#define RTL_REG_RBSTART  0x30u
#define RTL_REG_CR       0x37u
#define RTL_REG_CAPR     0x38u
#define RTL_REG_IMR      0x3Cu
#define RTL_REG_ISR      0x3Eu
#define RTL_REG_TCR      0x40u
#define RTL_REG_RCR      0x44u
#define RTL_REG_CONFIG1  0x52u
#define RTL_REG_MSR      0x58u

#define RTL_CR_RE        (1u << 3)
#define RTL_CR_TE        (1u << 2)
#define RTL_CR_RST       (1u << 4)
#define RTL_CR_BUFE      (1u << 0)

#define RTL_ISR_ROK      (1u << 0)
#define RTL_ISR_RER      (1u << 1)
#define RTL_ISR_TOK      (1u << 2)
#define RTL_ISR_TER      (1u << 3)
#define RTL_ISR_RXOVW    (1u << 4)

#define RTL_RCR_AAP      (1u << 0)
#define RTL_RCR_APM      (1u << 1)
#define RTL_RCR_AM       (1u << 2)
#define RTL_RCR_AB       (1u << 3)
#define RTL_RCR_WRAP     (1u << 7)

#define RTL_MSR_LINKB    (1u << 2)

#define RTL_RX_BUFFER_SIZE (8192u + 16u + 1500u)
#define RTL_TX_BUFFER_SIZE 2048u
#define RTL_TX_DESC_COUNT 4u
#define RTL_RX_RING_SIZE 8192u

typedef struct rtl8139_device {
    pci_device_info_t pci;
    uint32_t io_base;
    uint8_t irq;
    uint8_t mac[6];
    uint8_t tx_index;
    int ready;
    rtl8139_stats_t stats;
} rtl8139_device_t;

static rtl8139_device_t g_dev = {0};
static rtl8139_rx_callback_t g_rx_callback = 0;
static uint16_t g_rx_offset = 0;

static uint8_t g_rx_buffer[RTL_RX_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t g_tx_buffer[RTL_TX_DESC_COUNT][RTL_TX_BUFFER_SIZE] __attribute__((aligned(16)));

static inline uint8_t rtl_inb(uint16_t reg) {
    return inb((uint16_t)(g_dev.io_base + reg));
}

static inline uint16_t rtl_inw(uint16_t reg) {
    return inw((uint16_t)(g_dev.io_base + reg));
}

static inline uint32_t rtl_inl(uint16_t reg) {
    return inl((uint16_t)(g_dev.io_base + reg));
}

static inline void rtl_outb(uint16_t reg, uint8_t v) {
    outb((uint16_t)(g_dev.io_base + reg), v);
}

static inline void rtl_outw(uint16_t reg, uint16_t v) {
    outw((uint16_t)(g_dev.io_base + reg), v);
}

static inline void rtl_outl(uint16_t reg, uint32_t v) {
    outl((uint16_t)(g_dev.io_base + reg), v);
}

static uint16_t rtl_read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void rtl8139_drain_rx_ring(void) {
    while ((rtl_inb(RTL_REG_CR) & RTL_CR_BUFE) == 0u) {
        uint8_t* pkt = &g_rx_buffer[g_rx_offset];
        uint16_t rx_len = rtl_read_le16(pkt + 2);

        if (rx_len < 4u || rx_len > 1792u) {
            g_dev.stats.rx_drops++;
            break;
        }

        uint16_t frame_len = (uint16_t)(rx_len - 4u);
        if (g_rx_callback && frame_len > 0) {
            g_rx_callback(pkt + 4, frame_len);
        }

        g_dev.stats.rx_packets++;

        g_rx_offset = (uint16_t)((g_rx_offset + rx_len + 4u + 3u) & ~3u);
        g_rx_offset %= RTL_RX_RING_SIZE;
        rtl_outw(RTL_REG_CAPR, (uint16_t)(g_rx_offset - 16u));
    }
}

static void rtl8139_irq_handler(struct registers* regs) {
    (void)regs;

    if (!g_dev.ready) return;

    uint16_t isr = rtl_inw(RTL_REG_ISR);
    if (isr == 0) return;

    rtl_outw(RTL_REG_ISR, isr);
    g_dev.stats.interrupts++;

    if (isr & RTL_ISR_ROK) {
        g_dev.stats.rx_irqs++;
        rtl8139_drain_rx_ring();
    }
    if (isr & RTL_ISR_TOK) {
        g_dev.stats.tx_irqs++;
    }
    if (isr & (RTL_ISR_RER | RTL_ISR_TER | RTL_ISR_RXOVW)) {
        g_dev.stats.rx_drops++;
    }
}

static void rtl8139_read_mac(void) {
    for (uint32_t i = 0; i < 6; ++i) {
        g_dev.mac[i] = rtl_inb((uint16_t)(RTL_REG_IDR0 + i));
    }
}

int rtl8139_initialize(void) {
    if (g_dev.ready) return 0;

    if (pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &g_dev.pci) != 0) {
        return -1;
    }

    if (pci_enable_bus_mastering(&g_dev.pci) != 0) {
        klog("rtl8139: failed to enable PCI bus mastering");
        return -1;
    }

    g_dev.io_base = g_dev.pci.io_base;
    g_dev.irq = g_dev.pci.irq_line;

    if (g_dev.io_base == 0) {
        klog("rtl8139: no I/O BAR available");
        return -1;
    }

    rtl_outb(RTL_REG_CONFIG1, 0x00);
    rtl_outb(RTL_REG_CR, RTL_CR_RST);

    for (uint32_t i = 0; i < 200000u; ++i) {
        if ((rtl_inb(RTL_REG_CR) & RTL_CR_RST) == 0u) break;
        io_wait();
    }

    memset(g_rx_buffer, 0, sizeof(g_rx_buffer));
    rtl_outl(RTL_REG_RBSTART, (uint32_t)(uintptr_t)&g_rx_buffer[0]);

    memset(g_tx_buffer, 0, sizeof(g_tx_buffer));
    for (uint32_t i = 0; i < RTL_TX_DESC_COUNT; ++i) {
        rtl_outl((uint16_t)(RTL_REG_TSAD0 + i * 4u), (uint32_t)(uintptr_t)&g_tx_buffer[i][0]);
    }

    rtl_outl(RTL_REG_RCR, RTL_RCR_AAP | RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_AB | RTL_RCR_WRAP);
    rtl_outl(RTL_REG_TCR, 0x03000700u);

    rtl_outw(RTL_REG_ISR, 0xFFFFu);
    rtl_outw(RTL_REG_IMR, (uint16_t)(RTL_ISR_ROK | RTL_ISR_TOK | RTL_ISR_RER | RTL_ISR_TER | RTL_ISR_RXOVW));

    rtl_outb(RTL_REG_CR, (uint8_t)(RTL_CR_RE | RTL_CR_TE));

    rtl8139_read_mac();

    if (g_dev.irq < 16u) {
        irq_install_handler(g_dev.irq, rtl8139_irq_handler);
    }

    g_dev.tx_index = 0;
    g_rx_offset = 0;
    g_dev.ready = 1;
    return 0;
}

int rtl8139_is_ready(void) {
    return g_dev.ready;
}

int rtl8139_link_up(void) {
    if (!g_dev.ready) return 0;
    return (rtl_inb(RTL_REG_MSR) & RTL_MSR_LINKB) ? 1 : 0;
}

int rtl8139_send_raw(const void* data, uint16_t len) {
    if (!g_dev.ready || !data || len == 0 || len > RTL_TX_BUFFER_SIZE) return -1;

    uint32_t idx = g_dev.tx_index & (RTL_TX_DESC_COUNT - 1u);
    memcpy(&g_tx_buffer[idx][0], data, len);

    rtl_outl((uint16_t)(RTL_REG_TSD0 + idx * 4u), len);

    g_dev.tx_index = (uint8_t)((idx + 1u) & (RTL_TX_DESC_COUNT - 1u));
    g_dev.stats.tx_packets++;
    return 0;
}

void rtl8139_set_rx_callback(rtl8139_rx_callback_t cb) {
    g_rx_callback = cb;
}

int rtl8139_send_test_frame(void) {
    uint8_t frame[60] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x02, 0x13, 0x37, 0x00, 0x00, 0x01,
        0x88, 0xb5,
    };

    for (uint32_t i = 14; i < sizeof(frame); ++i) {
        frame[i] = (uint8_t)i;
    }

    return rtl8139_send_raw(frame, (uint16_t)sizeof(frame));
}

void rtl8139_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) return;
    for (uint32_t i = 0; i < 6; ++i) {
        out_mac[i] = g_dev.mac[i];
    }
}

void rtl8139_get_stats(rtl8139_stats_t* out_stats) {
    if (!out_stats) return;
    out_stats->interrupts = g_dev.stats.interrupts;
    out_stats->rx_packets = g_dev.stats.rx_packets;
    out_stats->tx_packets = g_dev.stats.tx_packets;
    out_stats->rx_irqs = g_dev.stats.rx_irqs;
    out_stats->tx_irqs = g_dev.stats.tx_irqs;
    out_stats->link_events = g_dev.stats.link_events;
    out_stats->rx_drops = g_dev.stats.rx_drops;
}

void rtl8139_get_debug_info(rtl8139_debug_info_t* out_info) {
    if (!out_info) return;

    out_info->vendor_id = g_dev.pci.vendor_id;
    out_info->device_id = g_dev.pci.device_id;
    out_info->io_base = g_dev.io_base;
    out_info->irq = g_dev.irq;

    if (!g_dev.ready) {
        out_info->command = 0;
        out_info->media_status = 0;
        return;
    }

    out_info->command = rtl_inb(RTL_REG_CR);
    out_info->media_status = rtl_inb(RTL_REG_MSR);
}

