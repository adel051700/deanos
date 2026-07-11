/* cls — clear the screen via the tty's ESC[2J handler. */
#include <unistd.h>

int main(void) {
    write(1, "\x1b[2J", 4);
    return 0;
}
