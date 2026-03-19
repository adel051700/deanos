#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include <stdint.h>

#define KSIGINT   2
#define KSIGKILL  9
#define KSIGTERM 15
#define KSIGCHLD 17

/* 1-based signal bit helpers for small pending/ignore masks. */
#define KSIG_BIT(sig) (1u << ((uint32_t)(sig) - 1u))

int signal_is_supported(int sig);
uint32_t signal_default_exit_status(int sig);

/* Send a signal to one process id or all tasks in a process group. */
int signal_send_task(int pid, int sig);
int signal_send_pgid(int pgid, int sig);

#endif
