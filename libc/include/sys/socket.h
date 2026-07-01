#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H 1

#include <stddef.h>
#include <netinet/in.h>

#ifndef _SSIZE_T_DECLARED
#define _SSIZE_T_DECLARED
typedef long ssize_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOL_SOCKET 1
#define SOL_TCP    6

#define SO_REUSEADDR 2
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

#define TCP_NODELAY 1

/* recv/send/sendto/recvfrom flags */
#define MSG_DONTWAIT 0x40

typedef uint32_t socklen_t;

int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr_in* addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr_in* addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen);
ssize_t send(int sockfd, const void* buf, size_t len, int flags);
ssize_t recv(int sockfd, void* buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void* buf, size_t len, int flags, const struct sockaddr_in* dest, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr_in* src, socklen_t* addrlen);
int shutdown(int sockfd, int how);
int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);
int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen);

/* Deprecated alias kept for one transition; prefer close(). */
int closesocket(int sockfd);

#ifdef __cplusplus
}
#endif

#endif

