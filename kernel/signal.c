#include "include/kernel/signal.h"
#include "include/kernel/task.h"

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

