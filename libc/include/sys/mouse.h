#ifndef _SYS_MOUSE_H
#define _SYS_MOUSE_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct mouse_state {
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
};

int get_mouse_state(struct mouse_state* out);
int mouse_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* _SYS_MOUSE_H */
