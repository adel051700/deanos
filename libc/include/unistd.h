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
#ifdef __cplusplus
}
#endif
#endif /* _UNISTD_H */
