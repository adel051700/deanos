#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include <stdint.h>

#define KSIG_MAX   31
#define KSIGINT   2
#define KSIGKILL  9
#define KSIGTERM 15
#define KSIGCHLD 17

#define KSIG_DFL ((uintptr_t)0u)
#define KSIG_IGN ((uintptr_t)1u)

/* 1-based signal bit helpers for small pending/ignore masks. */
#define KSIG_BIT(sig) (1u << ((uint32_t)(sig) - 1u))

typedef struct ksigaction {
	uintptr_t sa_handler;
	uint32_t  sa_flags;
	uint32_t  sa_mask;
	uintptr_t sa_restorer;
} ksigaction_t;

struct registers;

int signal_is_supported(int sig);
uint32_t signal_default_exit_status(int sig);

/* Send a signal to one process id or all tasks in a process group. */
int signal_send_task(int pid, int sig);
int signal_send_pgid(int pgid, int sig);
int signal_set_action_current(int sig, const ksigaction_t* act, ksigaction_t* oldact);
int signal_dispatch_current(struct registers* regs);
int signal_sigreturn_current(struct registers* regs);

#endif
