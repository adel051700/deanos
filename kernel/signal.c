#include "include/kernel/signal.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/task.h"
#include <string.h>

static int signal_can_install_handler(int sig) {
    return signal_is_supported(sig) && sig != KSIGKILL;
}

static int signal_pick_pending(task_t* t) {
    if (!t) return 0;
    for (int sig = 1; sig <= KSIG_MAX; ++sig) {
        uint32_t bit = KSIG_BIT(sig);
        if (!(t->pending_signals & bit)) continue;
        return sig;
    }
    return 0;
}

int signal_is_supported(int sig) {
    return (sig == KSIGINT || sig == KSIGKILL || sig == KSIGTERM || sig == KSIGCHLD);
}

uint32_t signal_default_exit_status(int sig) {
    return 128u + (uint32_t)sig;
}

int signal_send_task(int pid, int sig) {
    return task_send_signal(pid, sig);
}

int signal_send_pgid(int pgid, int sig) {
    return task_send_signal_pgid(pgid, sig);
}

int signal_set_action_current(int sig, const ksigaction_t* act, ksigaction_t* oldact) {
    task_t* t = task_current();
    if (!t) return -1;
    if (!signal_is_supported(sig)) return -1;
    if (sig <= 0 || sig > KSIG_MAX) return -1;

    uint32_t bit = KSIG_BIT(sig);
    uint32_t idx = (uint32_t)sig;

    if (oldact) {
        oldact->sa_handler = t->signal_handlers[idx];
        oldact->sa_flags = 0;
        oldact->sa_mask = 0;
        oldact->sa_restorer = t->signal_restorers[idx];
    }

    if (!act) return 0;
    if (!signal_can_install_handler(sig)) return -1;

    uintptr_t handler = act->sa_handler;
    if (handler > KSIG_IGN && act->sa_restorer == 0) return -1;

    t->signal_handlers[idx] = handler;
    t->signal_restorers[idx] = (handler > KSIG_IGN) ? act->sa_restorer : 0;

    if (handler == KSIG_IGN) {
        t->ignored_signals |= bit;
        t->pending_signals &= ~bit;
    } else {
        t->ignored_signals &= ~bit;
    }

    return 0;
}

int signal_dispatch_current(struct registers* regs) {
    task_t* t = task_current();
    if (!t || !regs) return -1;
    if ((regs->cs & 0x3u) != 0x3u) return 0;
    if (t->state == TASK_DEAD) return 0;
    if (t->signal_in_handler) return 0;

    int sig = signal_pick_pending(t);
    if (sig <= 0) return 0;

    uint32_t bit = KSIG_BIT(sig);
    uintptr_t handler = t->signal_handlers[(uint32_t)sig];
    if ((t->ignored_signals & bit) || handler == KSIG_IGN) {
        t->pending_signals &= ~bit;
        return 1;
    }

    if (handler <= KSIG_IGN) {
        if (sig == KSIGCHLD) {
            t->pending_signals &= ~bit;
            return 1;
        }
        return 0;
    }

    uintptr_t restorer = t->signal_restorers[(uint32_t)sig];
    if (restorer == 0) {
        t->pending_signals &= ~bit;
        return 0;
    }

    t->pending_signals &= ~bit;
    memcpy(&t->signal_saved_regs, regs, sizeof(struct registers));
    t->signal_in_handler = 1;
    t->signal_active = (uint32_t)sig;

    uint32_t new_sp = regs->useresp - 8u;
    uint32_t* usp = (uint32_t*)(uintptr_t)new_sp;
    usp[0] = (uint32_t)restorer;
    usp[1] = (uint32_t)sig;

    regs->useresp = new_sp;
    regs->eip = (uint32_t)handler;
    return 1;
}

int signal_sigreturn_current(struct registers* regs) {
    task_t* t = task_current();
    if (!t || !regs) return -1;
    if (!t->signal_in_handler) return -1;

    t->signal_in_handler = 0;
    t->signal_active = 0;
    memcpy(regs, &t->signal_saved_regs, sizeof(struct registers));
    return 0;
}

