/* /bin/vm — VM stats tool, ported from the kernel-shell builtin
 * (see docs/superpowers/specs/2026-07-14-kernel-shell-retirement-design.md).
 * Only `vm`/`vm stats` is ported; `demand`/`cow`/`refs` are kernel-internal
 * paging test hooks with no userspace equivalent (see spec Non-goals). */
#include <stdio.h>
#include <string.h>
#include <sys/vm.h>

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "stats") != 0) {
        printf("usage: vm stats\n");
        return 1;
    }

    struct vm_stats st;
    if (get_vm_stats(&st) != 0) {
        printf("vm: stats query failed\n");
        return 1;
    }

    printf("VM stats:\n");
    printf("  demand regions: %d\n", (int)st.demand_regions);
    printf("  demand faults:  %d\n", (int)st.demand_faults);
    printf("  COW faults:     %d\n", (int)st.cow_faults);
    printf("  swap enabled:   %s\n", st.swap_enabled ? "yes" : "no");
    printf("  swap slots:     %d/%d\n", (int)st.swap_slots_used, (int)st.swap_slots_total);
    printf("  swap page-outs: %d\n", (int)st.swap_pageouts);
    printf("  swap page-ins:  %d\n", (int)st.swap_pageins);
    printf("  swap faults:    %d\n", (int)st.swap_faults);
    return 0;
}
