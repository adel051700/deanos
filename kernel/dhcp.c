    #include "include/kernel/net_dhcp.h"

#include <string.h>

int net_dhcp_parse_options(const uint8_t* pkt,
                           uint16_t len,
                           uint8_t* out_msg_type,
                           uint8_t out_server_ip[4],
                           uint8_t out_mask[4],
                           uint8_t out_gw[4],
                           uint32_t* out_lease_s,
                           uint32_t* out_t1_s,
                           uint32_t* out_t2_s) {
    uint16_t off = 240u;

    if (!pkt || len < 244u || !out_msg_type || !out_server_ip || !out_mask || !out_gw ||
        !out_lease_s || !out_t1_s || !out_t2_s) {
        return -1;
    }

    *out_msg_type = 0u;
    memset(out_server_ip, 0, 4);
    out_mask[0] = 255u; out_mask[1] = 255u; out_mask[2] = 255u; out_mask[3] = 0u;
    memset(out_gw, 0, 4);
    *out_lease_s = 3600u;
    *out_t1_s = 0u;
    *out_t2_s = 0u;

    if (pkt[236] != 99u || pkt[237] != 130u || pkt[238] != 83u || pkt[239] != 99u) return -1;

    while (off < len) {
        uint8_t opt = pkt[off++];
        if (opt == 0u) continue;
        if (opt == 255u) break;
        if (off >= len) break;
        {
            uint8_t olen = pkt[off++];
            if ((uint32_t)off + olen > len) break;
            if (opt == 53u && olen == 1u) *out_msg_type = pkt[off];
            if (opt == 54u && olen == 4u) memcpy(out_server_ip, pkt + off, 4);
            if (opt == 1u && olen == 4u) memcpy(out_mask, pkt + off, 4);
            if (opt == 3u && olen >= 4u) memcpy(out_gw, pkt + off, 4);
            if (opt == 51u && olen == 4u) {
                *out_lease_s = ((uint32_t)pkt[off] << 24) |
                               ((uint32_t)pkt[off + 1u] << 16) |
                               ((uint32_t)pkt[off + 2u] << 8) |
                               (uint32_t)pkt[off + 3u];
            }
            if (opt == 58u && olen == 4u) {
                *out_t1_s = ((uint32_t)pkt[off] << 24) |
                            ((uint32_t)pkt[off + 1u] << 16) |
                            ((uint32_t)pkt[off + 2u] << 8) |
                            (uint32_t)pkt[off + 3u];
            }
            if (opt == 59u && olen == 4u) {
                *out_t2_s = ((uint32_t)pkt[off] << 24) |
                            ((uint32_t)pkt[off + 1u] << 16) |
                            ((uint32_t)pkt[off + 2u] << 8) |
                            (uint32_t)pkt[off + 3u];
            }
            off = (uint16_t)(off + olen);
        }
    }

    return 0;
}

