#include "include/kernel/net.h"

#include "include/kernel/e1000.h"
#include "include/kernel/log.h"
#include "include/kernel/pci.h"
#include "include/kernel/rtl8139.h"

typedef enum net_driver_kind {
    NET_DRIVER_NONE = 0,
    NET_DRIVER_E1000,
    NET_DRIVER_RTL8139,
} net_driver_kind_t;

static net_driver_kind_t g_driver = NET_DRIVER_NONE;

int net_initialize(void) {
    pci_initialize();

    if (e1000_initialize() == 0) {
        g_driver = NET_DRIVER_E1000;
        return 0;
    }

    if (rtl8139_initialize() == 0) {
        g_driver = NET_DRIVER_RTL8139;
        return 0;
    }

    g_driver = NET_DRIVER_NONE;
    klog("net: no supported NIC initialized");
    return -1;
}

int net_is_ready(void) {
    if (g_driver == NET_DRIVER_E1000) return e1000_is_ready();
    if (g_driver == NET_DRIVER_RTL8139) return rtl8139_is_ready();
    return 0;
}

int net_link_up(void) {
    if (g_driver == NET_DRIVER_E1000) return e1000_link_up();
    if (g_driver == NET_DRIVER_RTL8139) return rtl8139_link_up();
    return 0;
}

const char* net_driver_name(void) {
    if (g_driver == NET_DRIVER_E1000) return "e1000";
    if (g_driver == NET_DRIVER_RTL8139) return "rtl8139";
    return "none";
}

int net_send_test_frame(void) {
    if (g_driver == NET_DRIVER_E1000) return e1000_send_test_frame();
    if (g_driver == NET_DRIVER_RTL8139) return rtl8139_send_test_frame();
    return -1;
}

void net_get_mac(uint8_t out_mac[6]) {
    if (!out_mac) return;

    if (g_driver == NET_DRIVER_E1000) {
        e1000_get_mac(out_mac);
        return;
    }
    if (g_driver == NET_DRIVER_RTL8139) {
        rtl8139_get_mac(out_mac);
        return;
    }

    for (uint32_t i = 0; i < 6; ++i) out_mac[i] = 0;
}

void net_get_stats(net_stats_t* out_stats) {
    if (!out_stats) return;

    if (g_driver == NET_DRIVER_E1000) {
        e1000_stats_t st;
        e1000_get_stats(&st);
        out_stats->interrupts = st.interrupts;
        out_stats->rx_packets = st.rx_packets;
        out_stats->tx_packets = st.tx_packets;
        out_stats->rx_irqs = st.rx_irqs;
        out_stats->tx_irqs = st.tx_irqs;
        out_stats->link_events = st.link_events;
        out_stats->rx_drops = st.rx_drops;
        return;
    }

    if (g_driver == NET_DRIVER_RTL8139) {
        rtl8139_stats_t st;
        rtl8139_get_stats(&st);
        out_stats->interrupts = st.interrupts;
        out_stats->rx_packets = st.rx_packets;
        out_stats->tx_packets = st.tx_packets;
        out_stats->rx_irqs = st.rx_irqs;
        out_stats->tx_irqs = st.tx_irqs;
        out_stats->link_events = st.link_events;
        out_stats->rx_drops = st.rx_drops;
        return;
    }

    out_stats->interrupts = 0;
    out_stats->rx_packets = 0;
    out_stats->tx_packets = 0;
    out_stats->rx_irqs = 0;
    out_stats->tx_irqs = 0;
    out_stats->link_events = 0;
    out_stats->rx_drops = 0;
}

void net_get_debug_info(net_debug_info_t* out_info) {
    if (!out_info) return;

    if (g_driver == NET_DRIVER_E1000) {
        e1000_debug_info_t dbg;
        e1000_get_debug_info(&dbg);
        out_info->vendor_id = dbg.vendor_id;
        out_info->device_id = dbg.device_id;
        out_info->io_base = dbg.io_base;
        out_info->irq = dbg.irq;
        out_info->reg_a = dbg.ctrl;
        out_info->reg_b = dbg.status;
        out_info->reg_c = dbg.ral0;
        out_info->reg_d = dbg.rah0;
        return;
    }

    if (g_driver == NET_DRIVER_RTL8139) {
        rtl8139_debug_info_t dbg;
        rtl8139_get_debug_info(&dbg);
        out_info->vendor_id = dbg.vendor_id;
        out_info->device_id = dbg.device_id;
        out_info->io_base = dbg.io_base;
        out_info->irq = dbg.irq;
        out_info->reg_a = dbg.command;
        out_info->reg_b = dbg.media_status;
        out_info->reg_c = 0;
        out_info->reg_d = 0;
        return;
    }

    out_info->vendor_id = 0;
    out_info->device_id = 0;
    out_info->io_base = 0;
    out_info->irq = 0;
    out_info->reg_a = 0;
    out_info->reg_b = 0;
    out_info->reg_c = 0;
    out_info->reg_d = 0;
}

