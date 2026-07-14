/* /bin/dean — banner, ported from the kernel-shell builtin. Printed at
 * normal size: there is no syscall for text scale and this phase does not
 * add one solely for a decorative banner (see spec Non-goals). */
#include <stdio.h>

int main(void) {
    printf(" _____                    ____   _____ \n");
    printf("|  __ \\                  / __ \\ / ____|\n");
    printf("| |  | | ___  __ _ _ __ | |  | | (___  \n");
    printf("| |  | |/ _ \\/ _` | '_ \\| |  | |\\___ \\\n");
    printf("| |__| |  __/ (_| | | | | |__| |____) |\n");
    printf("|_____/ \\___|\\__,_|_| |_|\\____/|_____/\n");
    return 0;
}
