#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    char text[512];
    text[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(text, " ", sizeof(text) - strlen(text) - 1);
        strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
    }
    printf("%s\n", text);
    return 0;
}
