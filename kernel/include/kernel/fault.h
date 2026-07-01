#ifndef _KERNEL_FAULT_H
#define _KERNEL_FAULT_H

/* Install handlers for CPU faults that must be survivable when raised from
 * user mode (currently #GP, used by the non-executable user stack). */
void fault_initialize(void);

#endif
