#ifndef _UNISTD_H
#define _UNISTD_H 1
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef long ssize_t;

#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17

struct in_addr {
	uint8_t s_addr[4];
};

struct sockaddr_in {
	uint16_t sin_family;
	uint16_t sin_port;
	struct in_addr sin_addr;
};

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
int execve(const char* path);
int pipe(int pipefd[2]);
int wait(int* status);
int waitpid(int pid, int* status, int options);
int setpgid(int pid, int pgid);
int getpgrp(void);
int setsid(void);
int tcsetpgrp(int fd, int pgrp);
int tcgetpgrp(int fd);
int chmod(const char* path, uint16_t mode);
int chown(const char* path, uint32_t uid, uint32_t gid);
int socket(int domain, int type, int protocol);
int closesocket(int sockfd);
int bind(int sockfd, const struct sockaddr_in* addr);
ssize_t sendto(int sockfd, const void* buf, size_t len, const struct sockaddr_in* dest);
ssize_t recvfrom(int sockfd, void* buf, size_t len, struct sockaddr_in* src, unsigned timeout_ms);
#ifdef __cplusplus
}
#endif
#endif /* _UNISTD_H */
