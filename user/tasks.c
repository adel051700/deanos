/* tasks — list all tasks, ported from the kernel shell builtin. */
#include <stdio.h>
#include <unistd.h>
#include <sys/proc.h>

int main(void) {
    static const char* state_names[] = {"READY", "RUN  ", "BLOCK", "DEAD "};
    int self = getpid();

    printf("ID  PPID  SID  PGID  STATE  QUANTUM  NAME\n");
    printf("--  ----  ---  ----  -----  -------  ----\n");

    struct task_info info;
    for (unsigned i = 0; task_list(i, &info) == 0; i++) {
        printf("%s%d  %d    %d   %d    ",
               info.id == self ? "*" : " ",
               (int)info.id, (int)info.parent_id, (int)info.sid, (int)info.pgid);
        if (info.state >= 0 && info.state <= 3) printf("%s", state_names[info.state]);
        else printf("???? ");
        printf("  %d        %s\n", (int)info.quantum, info.name);
    }
    return 0;
}
