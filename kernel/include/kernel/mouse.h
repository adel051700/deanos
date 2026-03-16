#ifndef _KERNEL_MOUSE_H
#define _KERNEL_MOUSE_H

#include <stdint.h>

typedef struct mouse_state {
    int32_t x;
    int32_t y;
    int32_t dx_total;
    int32_t dy_total;
    uint8_t buttons;
    uint8_t x_overflow;
    uint8_t y_overflow;
    uint32_t packet_count;
    uint32_t left_clicks;
    uint32_t right_clicks;
    uint32_t middle_clicks;
} mouse_state_t;

void mouse_initialize(void);
int mouse_is_ready(void);
void mouse_get_state(mouse_state_t* out);
void mouse_clear_motion(void);
void mouse_reset_counters(void);

#endif

