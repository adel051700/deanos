#include "include/kernel/e1000.h"

#include "include/kernel/interrupt.h"
#include "include/kernel/io.h"
#include "include/kernel/irq.h"
#include "include/kernel/log.h"
#include "include/kernel/paging.h"
#include "include/kernel/pci.h"

#include <string.h>

#define E1000_VENDOR_ID 0x8086u

#define E1000_DEV_82540EM 0x100Eu
#define E1000_DEV_82545EM 0x100Fu
#define E1000_DEV_82546EB 0x1010u
#define E1000_DEV_I217    0x153Au

#define E1000_REG_CTRL    0x0000u
#define E1000_REG_STATUS  0x0008u
#define E1000_REG_EERD    0x0014u
#define E1000_REG_ICR     0x00C0u
#define E1000_REG_IMS     0x00D0u
#define E1000_REG_IMC     0x00D8u

#define E1000_REG_RCTL    0x0100u
#define E1000_REG_TCTL    0x0400u
#define E1000_REG_TIPG    0x0410u

#define E1000_REG_RDBAL   0x2800u
#define E1000_REG_RDBAH   0x2804u
#define E1000_REG_RDLEN   0x2808u
#define E1000_REG_RDH     0x2810u
#define E1000_REG_RDT     0x2818u

#define E1000_REG_TDBAL   0x3800u
#define E1000_REG_TDBAH   0x3804u
#define E1000_REG_TDLEN   0x3808u
#define E1000_REG_TDH     0x3810u
#define E1000_REG_TDT     0x3818u

#define E1000_REG_RAL0    0x5400u
#define E1000_REG_RAH0    0x5404u

#define E1000_CTRL_RST    (1u << 26)
#define E1000_CTRL_ASDE   (1u << 5)
#define E1000_CTRL_SLU    (1u << 6)

#define E1000_STATUS_LU   (1u << 1)

#define E1000_RAH_AV      (1u << 31)

#define E1000_EERD_START  (1u << 0)
#define E1000_EERD_DONE   (1u << 4)

#define E1000_IMS_TXDW    (1u << 0)
#define E1000_IMS_LSC     (1u << 2)
#define E1000_IMS_RXO     (1u << 6)
#define E1000_IMS_RXT0    (1u << 7)

#define E1000_RCTL_EN     (1u << 1)
#define E1000_RCTL_BAM    (1u << 15)
#define E1000_RCTL_SECRC  (1u << 26)

#define E1000_TCTL_EN     (1u << 1)
#define E1000_TCTL_PSP    (1u << 3)
#define E1000_TCTL_CT_SHIFT   4
#define E1000_TCTL_COLD_SHIFT 12

#define E1000_TX_CMD_EOP  (1u << 0)
#define E1000_TX_CMD_RS   (1u << 3)
#define E1000_TX_STAT_DD  (1u << 0)

#define E1000_RX_STAT_DD  (1u << 0)

#define E1000_TX_RING_SIZE 8u
#define E1000_RX_RING_SIZE 8u
#define E1000_PKT_BUF_SIZE 2048u

typedef struct __attribute__((packed)) e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} e1000_tx_desc_t;

typedef struct __attribute__((packed)) e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

static pci_device_info_t g_dev;
static uint32_t g_io_base = 0;
static volatile uint8_t* g_mmio_base = 0;
static int g_use_mmio = 0;
static uint8_t g_irq = 0xFFu;
static uint8_t g_mac[6] = {0};
static int g_ready = 0;

static volatile uint32_t g_tx_tail = 0;
static volatile uint32_t g_rx_tail = 0;
static e1000_rx_callback_t g_rx_callback = 0;

static volatile e1000_stats_t g_stats = {0};

static e1000_tx_desc_t g_tx_ring[E1000_TX_RING_SIZE] __attribute__((aligned(16)));
static e1000_rx_desc_t g_rx_ring[E1000_RX_RING_SIZE] __attribute__((aligned(16)));

static uint8_t g_tx_buf[E1000_TX_RING_SIZE][E1000_PKT_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_rx_buf[E1000_RX_RING_SIZE][E1000_PKT_BUF_SIZE] __attribute__((aligned(16)));

static uint32_t e1000_read(uint32_t reg) {
    if (g_use_mmio && g_mmio_base) {
        return *(volatile uint32_t*)(g_mmio_base + reg);
    }
    outl((uint16_t)g_io_base, reg);
    return inl((uint16_t)(g_io_base + 4u));
}

static void e1000_write(uint32_t reg, uint32_t value) {
    if (g_use_mmio && g_mmio_base) {
        *(volatile uint32_t*)(g_mmio_base + reg) = value;
        return;
    }
    outl((uint16_t)g_io_base, reg);
    outl((uint16_t)(g_io_base + 4u), value);
}

static void e1000_delay(uint32_t count) {
    while (count--) {
        io_wait();
    }
}

static int e1000_mac_is_zero(const uint8_t mac[6]) {
    return (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0;
}

static int e1000_wait_reset_done(void) {
    for (uint32_t i = 0; i < 200000u; ++i) {
        if ((e1000_read(E1000_REG_CTRL) & E1000_CTRL_RST) == 0u) {
            return 0;
        }
        io_wait();
    }
    return -1;
}

static int e1000_read_eeprom_word(uint8_t addr, uint16_t* out_word) {
    if (!out_word) return -1;

    e1000_write(E1000_REG_EERD, E1000_EERD_START | ((uint32_t)addr << 8));
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint32_t v = e1000_read(E1000_REG_EERD);
        if (v & E1000_EERD_DONE) {
            *out_word = (uint16_t)((v >> 16) & 0xFFFFu);
            return 0;
        }
        io_wait();
    }

    return -1;
}

static void e1000_irq_handler(struct registers* regs) {
    (void)regs;

    if (!g_ready) return;

    uint32_t icr = e1000_read(E1000_REG_ICR);
    if (icr == 0) return;

    g_stats.interrupts++;

    if (icr & E1000_IMS_LSC) {
        g_stats.link_events++;
        klog(e1000_link_up() ? "e1000: link up" : "e1000: link down");
    }
    if (icr & E1000_IMS_TXDW) {
        g_stats.tx_irqs++;
    }
    if (icr & E1000_IMS_RXT0) {
        g_stats.rx_irqs++;
    }
    if (icr & E1000_IMS_RXO) {
        g_stats.rx_drops++;
    }

    for (;;) {
        uint32_t idx = g_rx_tail;
        e1000_rx_desc_t* d = &g_rx_ring[idx];
        if ((d->status & E1000_RX_STAT_DD) == 0u) {
            break;
        }

        g_stats.rx_packets++;
        if (g_rx_callback && d->length >= 14u && d->length <= E1000_PKT_BUF_SIZE) {
            g_rx_callback(&g_rx_buf[idx][0], d->length);
        }
        d->status = 0;
        e1000_write(E1000_REG_RDT, idx);
        g_rx_tail = (idx + 1u) % E1000_RX_RING_SIZE;
    }
}

static int e1000_find_device(pci_device_info_t* out) {
    static const uint16_t ids[] = {
        E1000_DEV_82540EM,
        E1000_DEV_82545EM,
        E1000_DEV_82546EB,
        E1000_DEV_I217,
    };

    for (uint32_t i = 0; i < (uint32_t)(sizeof(ids) / sizeof(ids[0])); ++i) {
        if (pci_find_device(E1000_VENDOR_ID, ids[i], out) == 0) {
            return 0;
        }
    }

    return -1;
}

static void e1000_setup_tx(void) {
    memset(g_tx_ring, 0, sizeof(g_tx_ring));
    memset(g_tx_buf, 0, sizeof(g_tx_buf));

    for (uint32_t i = 0; i < E1000_TX_RING_SIZE; ++i) {
        g_tx_ring[i].addr = (uint64_t)(uintptr_t)&g_tx_buf[i][0];
        g_tx_ring[i].status = E1000_TX_STAT_DD;
    }

    e1000_write(E1000_REG_TDBAL, (uint32_t)(uintptr_t)&g_tx_ring[0]);
    e1000_write(E1000_REG_TDBAH, 0);
    e1000_write(E1000_REG_TDLEN, (uint32_t)sizeof(g_tx_ring));
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);

    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                    (0x10u << E1000_TCTL_CT_SHIFT) |
                    (0x40u << E1000_TCTL_COLD_SHIFT);
    e1000_write(E1000_REG_TCTL, tctl);
    e1000_write(E1000_REG_TIPG, 0x0060200Au);

    g_tx_tail = 0;
}

static void e1000_setup_rx(void) {
    memset(g_rx_ring, 0, sizeof(g_rx_ring));
    memset(g_rx_buf, 0, sizeof(g_rx_buf));

    for (uint32_t i = 0; i < E1000_RX_RING_SIZE; ++i) {
        g_rx_ring[i].addr = (uint64_t)(uintptr_t)&g_rx_buf[i][0];
        g_rx_ring[i].status = 0;
    }

    e1000_write(E1000_REG_RDBAL, (uint32_t)(uintptr_t)&g_rx_ring[0]);
    e1000_write(E1000_REG_RDBAH, 0);
    e1000_write(E1000_REG_RDLEN, (uint32_t)sizeof(g_rx_ring));
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, E1000_RX_RING_SIZE - 1u);

    e1000_write(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);

    g_rx_tail = 0;
}

static void e1000_read_mac(void) {
    uint32_t ral = e1000_read(E1000_REG_RAL0);
    uint32_t rah = e1000_read(E1000_REG_RAH0);

    if ((rah & E1000_RAH_AV) != 0u) {
        g_mac[0] = (uint8_t)(ral & 0xFFu);
        g_mac[1] = (uint8_t)((ral >> 8) & 0xFFu);
        g_mac[2] = (uint8_t)((ral >> 16) & 0xFFu);
        g_mac[3] = (uint8_t)((ral >> 24) & 0xFFu);
        g_mac[4] = (uint8_t)(rah & 0xFFu);
        g_mac[5] = (uint8_t)((rah >> 8) & 0xFFu);
        if (!e1000_mac_is_zero(g_mac)) {
            return;
        }
    }

    for (uint32_t i = 0; i < 3; ++i) {
        uint16_t w = 0;
        if (e1000_read_eeprom_word((uint8_t)i, &w) != 0) {
            break;
        }
        g_mac[i * 2] = (uint8_t)(w & 0xFFu);
        g_mac[i * 2 + 1] = (uint8_t)((w >> 8) & 0xFFu);
    }
}

int e1000_initialize(void) {
    if (g_ready) return 0;

    if (e1000_find_device(&g_dev) != 0) {
        return -1;
    }

    if (pci_enable_bus_mastering(&g_dev) != 0) {
        klog("e1000: failed to enable PCI bus mastering");
        return -1;
    }

    if (g_dev.mmio_base != 0 && paging_identity_map_mmio((uintptr_t)g_dev.mmio_base, 0x20000u) == 0) {
        g_mmio_base = (volatile uint8_t*)(uintptr_t)g_dev.mmio_base;
        g_use_mmio = 1;
    } else {
        g_mmio_base = 0;
        g_use_mmio = 0;
    }

    g_io_base = g_dev.io_base;
    g_irq = g_dev.irq_line;

    if (!g_use_mmio && g_io_base == 0) {
        klog("e1000: no usable MMIO or I/O BAR");
        return -1;
    }

    e1000_write(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)e1000_read(E1000_REG_ICR);

    uint32_t ctrl = e1000_read(E1000_REG_CTRL);
    e1000_write(E1000_REG_CTRL, ctrl | E1000_CTRL_RST);
    if (e1000_wait_reset_done() != 0) {
        klog("e1000: reset timed out");
        return -1;
    }

    /* Explicitly request link/auto-speed after reset. */
    ctrl = e1000_read(E1000_REG_CTRL);
    e1000_write(E1000_REG_CTRL, ctrl | E1000_CTRL_ASDE | E1000_CTRL_SLU);
    e1000_delay(2000);

    e1000_setup_tx();
    e1000_setup_rx();
    e1000_read_mac();

    e1000_write(E1000_REG_IMS, E1000_IMS_TXDW | E1000_IMS_RXT0 | E1000_IMS_RXO | E1000_IMS_LSC);

    if (g_irq < 16u) {
        irq_install_handler(g_irq, e1000_irq_handler);
    }

    g_ready = 1;

    return 0;
}

int e1000_is_ready(void) {
    return g_ready;
}

int e1000_link_up(void) {
    if (!g_ready) return 0;
    return (e1000_read(E1000_REG_STATUS) & E1000_STATUS_LU) ? 1 : 0;
}

void e1000_set_rx_callback(e1000_rx_callback_t callback) {
    g_rx_callback = callback;
}

int e1000_send_raw(const void* data, uint16_t len) {
    if (!g_ready || !data || len == 0 || len > E1000_PKT_BUF_SIZE) return -1;

    uint32_t idx = g_tx_tail;
    e1000_tx_desc_t* d = &g_tx_ring[idx];
    if ((d->status & E1000_TX_STAT_DD) == 0u) {
        return -2;
    }

    memcpy(&g_tx_buf[idx][0], data, len);

    d->length = len;
    d->cmd = (uint8_t)(E1000_TX_CMD_EOP | E1000_TX_CMD_RS);
    d->status = 0;

    g_tx_tail = (idx + 1u) % E1000_TX_RING_SIZE;
    e1000_write(E1000_REG_TDT, g_tx_tail);

    g_stats.tx_packets++;
    return 0;
}

int e1000_send_test_frame(void) {
    uint8_t frame[60] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x88, 0xb5,
    };

    for (uint32_t i = 14; i < sizeof(frame); ++i) {
        frame[i] = (uint8_t)i;
    }

    return e1000_send_raw(frame, (uint16_t)sizeof(frame));
}

void e1000_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) return;
    if (g_ready && e1000_mac_is_zero(g_mac)) {
        e1000_read_mac();
    }
    for (uint32_t i = 0; i < 6; ++i) {
        out_mac[i] = g_mac[i];
    }
}

void e1000_get_stats(e1000_stats_t* out_stats) {
    if (!out_stats) return;
    out_stats->interrupts = g_stats.interrupts;
    out_stats->rx_packets = g_stats.rx_packets;
    out_stats->tx_packets = g_stats.tx_packets;
    out_stats->rx_irqs = g_stats.rx_irqs;
    out_stats->tx_irqs = g_stats.tx_irqs;
    out_stats->link_events = g_stats.link_events;
    out_stats->rx_drops = g_stats.rx_drops;
}

void e1000_get_debug_info(e1000_debug_info_t* out_info) {
    if (!out_info) return;

    out_info->vendor_id = g_dev.vendor_id;
    out_info->device_id = g_dev.device_id;
    out_info->io_base = g_io_base;
    out_info->irq = g_irq;

    if (!g_ready) {
        out_info->ctrl = 0;
        out_info->status = 0;
        out_info->ral0 = 0;
        out_info->rah0 = 0;
        return;
    }

    out_info->ctrl = e1000_read(E1000_REG_CTRL);
    out_info->status = e1000_read(E1000_REG_STATUS);
    out_info->ral0 = e1000_read(E1000_REG_RAL0);
    out_info->rah0 = e1000_read(E1000_REG_RAH0);
}

