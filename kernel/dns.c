#include "include/kernel/net_dns.h"

#include <string.h>

int net_dns_encode_qname(const char* hostname, uint8_t* out, uint16_t out_capacity, uint16_t* out_len) {
	uint16_t pos = 0;
	uint16_t label_start = 0;
	uint16_t i = 0;

	if (!hostname || !out || !out_len || out_capacity < 2u) return -1;
	if (hostname[0] == '\0') return -1;

	while (hostname[i] != '\0') {
		if (hostname[i] == '.') {
			uint16_t label_len = (uint16_t)(i - label_start);
			if (label_len == 0u || label_len > 63u) return -1;
			if ((uint16_t)(pos + 1u + label_len) >= out_capacity) return -1;
			out[pos++] = (uint8_t)label_len;
			memcpy(out + pos, hostname + label_start, label_len);
			pos = (uint16_t)(pos + label_len);
			label_start = (uint16_t)(i + 1u);
		}
		i++;
	}

	if (i == label_start) return -1;
	{
		uint16_t label_len = (uint16_t)(i - label_start);
		if (label_len == 0u || label_len > 63u) return -1;
		if ((uint16_t)(pos + 1u + label_len + 1u) > out_capacity) return -1;
		out[pos++] = (uint8_t)label_len;
		memcpy(out + pos, hostname + label_start, label_len);
		pos = (uint16_t)(pos + label_len);
	}

	out[pos++] = 0u;
	*out_len = pos;
	return 0;
}

int net_dns_skip_name(const uint8_t* msg, uint16_t msg_len, uint16_t start_off, uint16_t* out_next) {
	uint16_t off = start_off;
	uint16_t steps = 0;

	if (!msg || !out_next || off >= msg_len) return -1;

	while (off < msg_len && steps < msg_len) {
		uint8_t len = msg[off];
		if ((len & 0xC0u) == 0xC0u) {
			if ((uint16_t)(off + 1u) >= msg_len) return -1;
			*out_next = (uint16_t)(off + 2u);
			return 0;
		}
		if (len == 0u) {
			*out_next = (uint16_t)(off + 1u);
			return 0;
		}
		if ((len & 0xC0u) != 0u) return -1;
		off = (uint16_t)(off + 1u + len);
		if (off > msg_len) return -1;
		steps++;
	}

	return -1;
}

