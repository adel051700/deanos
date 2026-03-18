#ifndef _UNISTD_H
#define _UNISTD_H 1
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef long ssize_t;
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);
void _exit(int status);
int sched_yield(void);
unsigned sleep(unsigned seconds);
int sleep_ms(unsigned milliseconds);
int getpid(void);
int getppid(void);
int kill(int pid);
int fork(void);
int execve(const char* path);
int pipe(int pipefd[2]);
int wait(int* status);
int waitpid(int pid, int* status, int options);
int setpgid(int pid, int pgid);
int getpgrp(void);
int setsid(void);
int tcsetpgrp(int fd, int pgrp);
int tcgetpgrp(int fd);
#ifdef __cplusplus
}
#endif
#endif /* _UNISTD_H */
