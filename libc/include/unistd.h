#ifndef _UNISTD_H
#define _UNISTD_H 1
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef long ssize_t;

/* Legacy note: socket()/bind()/connect()/... used to be declared here with
 * ad hoc struct in_addr/sockaddr_in copies. That duplicated (and, after the
 * Task 11 ABI cleanup, conflicted with) the canonical declarations in
 * <sys/socket.h> and <netinet/in.h>. Callers needing sockets should include
 * <sys/socket.h> directly, per standard practice. */

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);
void _exit(int status);
int sched_yield(void);
unsigned sleep(unsigned seconds);
int sleep_ms(unsigned milliseconds);
int getpid(void);
int getppid(void);
int getuid(void);
int getgid(void);
int kill(int pid, int sig);
int fork(void);
int execve(const char* path, char* const argv[]);
int pipe(int pipefd[2]);
int dup2(int oldfd, int newfd);
int wait(int* status);
int waitpid(int pid, int* status, int options);
int setpgid(int pid, int pgid);
int getpgrp(void);
int setsid(void);
int tcsetpgrp(int fd, int pgrp);
int tcgetpgrp(int fd);
int chmod(const char* path, uint16_t mode);
int chown(const char* path, uint32_t uid, uint32_t gid);
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int unlink(const char* path);
long getrandom(void* buf, unsigned long len, unsigned int flags);
#define GRND_NONBLOCK 0x0001u
#define GRND_RANDOM   0x0002u
#ifdef __cplusplus
}
#endif
#endif /* _UNISTD_H */
