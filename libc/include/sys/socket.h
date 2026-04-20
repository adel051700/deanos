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

typedef uint32_t socklen_t;

int socket(int domain, int type, int protocol);
int closesocket(int sockfd);
int bind(int sockfd, const struct sockaddr_in* addr);
int connect(int sockfd, const struct sockaddr_in* addr, unsigned timeout_ms);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr_in* addr, unsigned timeout_ms);
ssize_t send(int sockfd, const void* buf, size_t len, unsigned timeout_ms);
ssize_t recv(int sockfd, void* buf, size_t len, unsigned timeout_ms);
ssize_t sendto(int sockfd, const void* buf, size_t len, const struct sockaddr_in* dest);
ssize_t recvfrom(int sockfd, void* buf, size_t len, struct sockaddr_in* src, unsigned timeout_ms);
int shutdown(int sockfd, int how);
int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);
int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen);

#ifdef __cplusplus
}
#endif

#endif

