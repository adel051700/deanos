#include <fcntl.h>
#include <unistd.h>
#include <sys/rtc.h>
#include <sys/proc.h>
#include <sys/mouse.h>
#include <sys/vmstat.h>

static void write_str(int fd, const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    write(fd, s, len);
}

static void write_int(int fd, int v) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    int neg = v < 0;
    unsigned uv = neg ? (unsigned)(-(v + 1)) + 1u : (unsigned)v;
    do {
        buf[--i] = (char)('0' + (uv % 10u));
        uv /= 10u;
    } while (uv != 0u);
    if (neg) buf[--i] = '-';
    write_str(fd, &buf[i]);
}

static void write_uint(int fd, unsigned v) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    write_str(fd, &buf[i]);
}

static void write_field(int fd, const char* label, unsigned v) {
    write_str(fd, label);
    write_uint(fd, v);
    write_str(fd, "\n");
}

int main(void) {
    int fd = open("/syscalltest.out", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 1;

    struct rtc_time rt;
    if (get_localtime(&rt) == 0) {
        write_str(fd, "localtime=");
        write_uint(fd, rt.year); write_str(fd, "-");
        write_uint(fd, rt.month); write_str(fd, "-");
        write_uint(fd, rt.day); write_str(fd, " ");
        write_uint(fd, rt.hour); write_str(fd, ":");
        write_uint(fd, rt.minute); write_str(fd, ":");
        write_uint(fd, rt.second); write_str(fd, "\n");
    } else {
        write_str(fd, "localtime=ERROR\n");
    }

    int original_tz = tz_get();
    write_field(fd, "tz_before=", (unsigned)original_tz);
    int set_rc = tz_set(3);
    write_field(fd, "tz_set_rc=", (unsigned)set_rc);
    write_field(fd, "tz_after_set=", (unsigned)tz_get());
    tz_set(original_tz); /* restore, best-effort, don't leave test state behind */
    write_field(fd, "tz_after_restore=", (unsigned)tz_get());

    struct uptime up;
    if (uptime(&up) == 0) {
        write_field(fd, "uptime_days=", up.days);
        write_field(fd, "uptime_hours=", up.hours);
        write_field(fd, "uptime_minutes=", up.minutes);
        write_field(fd, "uptime_seconds=", up.seconds);
    } else {
        write_str(fd, "uptime=ERROR\n");
    }

    struct pit_stats ps;
    if (get_pit_stats(&ps) == 0) {
        write_field(fd, "pit_ticks=", (unsigned)ps.ticks);
        write_field(fd, "pit_uptime_ms=", (unsigned)ps.uptime_ms);
    } else {
        write_str(fd, "pit_stats=ERROR\n");
    }

    unsigned idx = 0;
    unsigned task_n = 0;
    struct task_info ti;
    while (task_list(idx, &ti) == 0) {
        write_str(fd, "task[");
        write_uint(fd, idx);
        write_str(fd, "].id=");
        write_int(fd, ti.id);
        write_str(fd, " parent=");
        write_int(fd, ti.parent_id);
        write_str(fd, " sid=");
        write_int(fd, ti.sid);
        write_str(fd, " pgid=");
        write_int(fd, ti.pgid);
        write_str(fd, " state=");
        write_int(fd, ti.state);
        write_str(fd, " quantum=");
        write_uint(fd, ti.quantum);
        write_str(fd, " name=");
        write_str(fd, ti.name);
        write_str(fd, "\n");
        idx++;
        task_n++;
    }
    write_field(fd, "task_count=", task_n);

    struct mouse_state ms;
    int ready = get_mouse_state(&ms);
    write_field(fd, "mouse_ready=", (unsigned)ready);
    write_field(fd, "mouse_packet_count=", ms.packet_count);
    write_field(fd, "mouse_buttons=", ms.buttons);
    int reset_rc = mouse_reset();
    write_field(fd, "mouse_reset_rc=", (unsigned)reset_rc);

    struct vm_stats vs;
    if (get_vm_stats(&vs) == 0) {
        write_field(fd, "vm_demand_regions=", vs.demand_regions);
        write_field(fd, "vm_demand_faults=", vs.demand_faults);
        write_field(fd, "vm_cow_faults=", vs.cow_faults);
        write_field(fd, "vm_swap_slots_total=", vs.swap_slots_total);
        write_field(fd, "vm_swap_enabled=", vs.swap_enabled);
    } else {
        write_str(fd, "vm_stats=ERROR\n");
    }

    close(fd);
    return 0;
}
