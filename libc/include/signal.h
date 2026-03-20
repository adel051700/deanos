#ifndef _SIGNAL_H
#define _SIGNAL_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t sigset_t;
typedef void (*sighandler_t)(int);

#define SIGINT   2
#define SIGKILL  9
#define SIGTERM 15
#define SIGCHLD 17

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

struct sigaction {
    sighandler_t sa_handler;
    uint32_t sa_flags;
    sigset_t sa_mask;
    void (*sa_restorer)(void);
};

int kill(int pid, int sig);
int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact);
sighandler_t signal(int signum, sighandler_t handler);

#ifdef __cplusplus
}
#endif

#endif /* _SIGNAL_H */

