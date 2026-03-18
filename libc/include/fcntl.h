#ifndef _FCNTL_H
#define _FCNTL_H 1
#ifdef __cplusplus
extern "C" {
#endif
#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR   0x02
#define O_CREAT  0x04
#define O_TRUNC  0x08
#define O_APPEND 0x10
#define O_CLOEXEC 0x20

#define FD_CLOEXEC 0x1
#define F_GETFD    1
#define F_SETFD    2

int open(const char* path, int flags);
int fcntl(int fd, int cmd, int arg);
int mkdir(const char* path);
#ifdef __cplusplus
}
#endif
#endif /* _FCNTL_H */
