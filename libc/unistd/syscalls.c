#include <fcntl.h>
#include <kernel/syscall.h>
#include <signal.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((naked, noreturn)) static void __signal_restorer(void) {
    __asm__ volatile(
        "add $4, %%esp\n"
        "mov %[nr], %%eax\n"
        "int $0x80\n"
        "1: hlt\n"
        "jmp 1b\n"
        :
        : [nr] "i" (SYS_sigreturn)
        : "eax", "memory");
}

static inline long syscall1(unsigned num, unsigned a1) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1) : "memory");
    return ret;
}

static inline long syscall2(unsigned num, unsigned a1, unsigned a2) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2) : "memory");
    return ret;
}

static inline long syscall3(unsigned num, unsigned a1, unsigned a2, unsigned a3) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

ssize_t read(int fd, void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_read, (unsigned)fd, (unsigned)buf, (unsigned)count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_write, (unsigned)fd, (unsigned)buf, (unsigned)count);
}

int close(int fd) {
    return (int)syscall1(SYS_close, (unsigned)fd);
}

int open(const char* path, int flags) {
    return (int)syscall2(SYS_open, (unsigned)path, (unsigned)flags);
}

int fcntl(int fd, int cmd, int arg) {
    return (int)syscall3(SYS_fcntl, (unsigned)fd, (unsigned)cmd, (unsigned)arg);
}

int fstat(int fd, struct stat* st) {
    return (int)syscall2(SYS_fstat, (unsigned)fd, (unsigned)st);
}

int mkdir(const char* path) {
    return (int)syscall1(SYS_mkdir, (unsigned)path);
}

int sched_yield(void) {
    return (int)syscall1(SYS_yield, 0);
}

int sleep_ms(unsigned milliseconds) {
    return (int)syscall1(SYS_sleep_ms, milliseconds);
}

int getpid(void) {
    return (int)syscall1(SYS_getpid, 0);
}

int getppid(void) {
    return (int)syscall1(SYS_getppid, 0);
}

int kill(int pid, int sig) {
    return (int)syscall2(SYS_kill, (unsigned)pid, (unsigned)sig);
}

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    struct sigaction tmp;
    const struct sigaction* use_act = act;

    if (act && act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN && act->sa_restorer == 0) {
        tmp = *act;
        tmp.sa_restorer = __signal_restorer;
        use_act = &tmp;
    }

    return (int)syscall3(SYS_sigaction, (unsigned)signum, (unsigned)use_act, (unsigned)oldact);
}

sighandler_t signal(int signum, sighandler_t handler) {
    return (sighandler_t)syscall3(SYS_signal,
                                  (unsigned)signum,
                                  (unsigned)handler,
                                  (unsigned)__signal_restorer);
}

int fork(void) {
    return (int)syscall1(SYS_fork, 0);
}

int execve(const char* path) {
    return (int)syscall1(SYS_execve, (unsigned)path);
}

int pipe(int pipefd[2]) {
    return (int)syscall1(SYS_pipe, (unsigned)pipefd);
}

int wait(int* status) {
    return (int)syscall3(SYS_waitpid, (unsigned)-1, (unsigned)status, 0u);
}

int waitpid(int pid, int* status, int options) {
    return (int)syscall3(SYS_waitpid, (unsigned)pid, (unsigned)status, (unsigned)options);
}

int setpgid(int pid, int pgid) {
    return (int)syscall2(SYS_setpgid, (unsigned)pid, (unsigned)pgid);
}

int getpgrp(void) {
    return (int)syscall1(SYS_getpgrp, 0);
}

int setsid(void) {
    return (int)syscall1(SYS_setsid, 0);
}

int tcsetpgrp(int fd, int pgrp) {
    return (int)syscall2(SYS_tcsetpgrp, (unsigned)fd, (unsigned)pgrp);
}

int tcgetpgrp(int fd) {
    return (int)syscall1(SYS_tcgetpgrp, (unsigned)fd);
}

unsigned sleep(unsigned seconds) {
    (void)syscall1(SYS_sleep_ms, seconds * 1000u);
    return 0;
}

void _exit(int status) {
    (void)syscall1(SYS_exit, (unsigned)status);
    for (;;) {
        __asm__ volatile("hlt");
    }
}


