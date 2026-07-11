/* kill — send a signal to a pid.
 * Usage: kill [-INT|-TERM|-KILL|-CHLD|-<num>] <pid>   (default: -TERM) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static int parse_signal(const char* tok) {
    if (tok[0] >= '0' && tok[0] <= '9') return atoi(tok);
    if (strncmp(tok, "SIG", 3) == 0) tok += 3;
    if (strcmp(tok, "INT") == 0)  return SIGINT;
    if (strcmp(tok, "TERM") == 0) return SIGTERM;
    if (strcmp(tok, "KILL") == 0) return SIGKILL;
    if (strcmp(tok, "CHLD") == 0) return SIGCHLD;
    return -1;
}

int main(int argc, char** argv) {
    int sig = SIGTERM;
    int argi = 1;

    if (argi < argc && argv[argi][0] == '-') {
        sig = parse_signal(argv[argi] + 1);
        if (sig <= 0) {
            printf("kill: invalid signal (use INT, TERM, KILL, CHLD, or number)\n");
            return 1;
        }
        argi++;
    }
    if (argi >= argc) {
        printf("usage: kill [-INT|-TERM|-KILL|-CHLD|-<num>] <pid>\n");
        return 1;
    }

    int pid = atoi(argv[argi]);
    if (pid <= 0) {
        printf("kill: invalid pid\n");
        return 1;
    }
    if (kill(pid, sig) < 0) {
        printf("kill: failed to signal pid %d\n", pid);
        return 1;
    }
    return 0;
}
