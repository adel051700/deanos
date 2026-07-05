#ifndef _SYS_VMSTAT_H
#define _SYS_VMSTAT_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct vm_stats {
    uint32_t demand_regions;
    uint32_t demand_faults;
    uint32_t cow_faults;
    uint32_t swap_slots_total;
    uint32_t swap_slots_used;
    uint32_t swap_pageouts;
    uint32_t swap_pageins;
    uint32_t swap_faults;
    uint32_t swap_enabled;
};

int get_vm_stats(struct vm_stats* out);

#ifdef __cplusplus
}
#endif
#endif /* _SYS_VMSTAT_H */
