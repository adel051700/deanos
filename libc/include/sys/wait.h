#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H 1

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG 1

int wait(int* status);
int waitpid(int pid, int* status, int options);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_WAIT_H */

