/* /bin/mouse — PS/2 mouse state tool, ported from the kernel-shell builtin
 * (see docs/superpowers/specs/2026-07-14-kernel-shell-retirement-design.md). */
#include <stdio.h>
#include <string.h>
#include <sys/mouse.h>

int main(int argc, char** argv) {
    if (argc > 1 && (strcmp(argv[1], "clear") == 0 || strcmp(argv[1], "reset") == 0)) {
        mouse_reset();
        printf("mouse: counters cleared\n");
        return 0;
    }
    if (argc > 1) {
        printf("usage: mouse [clear]\n");
        return 1;
    }

    struct mouse_state st;
    int ready = get_mouse_state(&st);
    if (ready < 0) {
        printf("mouse: state query failed\n");
        return 1;
    }

    printf("mouse ready: %s\n", ready ? "yes" : "no");
    printf("packets: %d\n", (int)st.packet_count);
    printf("buttons: %d (L=%d M=%d R=%d)\n", st.buttons,
           (st.buttons & 0x1) ? 1 : 0, (st.buttons & 0x4) ? 1 : 0, (st.buttons & 0x2) ? 1 : 0);
    printf("pos: x=%d y=%d\n", (int)st.x, (int)st.y);
    printf("motion total: dx=%d dy=%d\n", (int)st.dx_total, (int)st.dy_total);
    printf("overflow: x=%d y=%d\n", st.x_overflow ? 1 : 0, st.y_overflow ? 1 : 0);
    printf("clicks: L=%d M=%d R=%d\n", (int)st.left_clicks, (int)st.middle_clicks, (int)st.right_clicks);
    return 0;
}
