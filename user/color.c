/* /bin/color — set terminal fg/bg color via SGR escapes, ported from the
 * kernel-shell builtin (see docs/superpowers/specs/2026-07-14-kernel-shell-retirement-design.md). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int parse_color_token(const char* token, unsigned* out_color) {
    if (!token || !*token) return 0;

    if (strcmp(token, "black") == 0)   { *out_color = 0x000000; return 1; }
    if (strcmp(token, "white") == 0)   { *out_color = 0xFFFFFF; return 1; }
    if (strcmp(token, "red") == 0)     { *out_color = 0xFF0000; return 1; }
    if (strcmp(token, "green") == 0)   { *out_color = 0x00FF00; return 1; }
    if (strcmp(token, "blue") == 0)    { *out_color = 0x0000FF; return 1; }
    if (strcmp(token, "yellow") == 0)  { *out_color = 0xFFFF00; return 1; }
    if (strcmp(token, "cyan") == 0)    { *out_color = 0x00FFFF; return 1; }
    if (strcmp(token, "magenta") == 0) { *out_color = 0xFF00FF; return 1; }
    if (strcmp(token, "gray") == 0 || strcmp(token, "grey") == 0) { *out_color = 0x808080; return 1; }

    const char* p = token;
    if (*p == '#') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

    unsigned value = 0;
    for (int i = 0; i < 6; ++i) {
        int nib = hex_nibble(p[i]);
        if (nib < 0) return 0;
        value = (value << 4) | (unsigned)nib;
    }
    if (p[6] != '\0') return 0;

    *out_color = value;
    return 1;
}

static void emit_sgr(int is_bg, unsigned color) {
    char buf[24];
    unsigned r = (color >> 16) & 0xFFu;
    unsigned g = (color >> 8) & 0xFFu;
    unsigned b = color & 0xFFu;
    int n = 0;
    buf[n++] = '\x1b';
    buf[n++] = '[';
    buf[n++] = is_bg ? '4' : '3';
    buf[n++] = '8';
    buf[n++] = ';';
    buf[n++] = '2';
    buf[n++] = ';';
    /* r/g/b are always 0-255, so up to 3 decimal digits each */
    unsigned vals[3] = { r, g, b };
    for (int i = 0; i < 3; ++i) {
        char tmp[4];
        int tn = 0;
        unsigned v = vals[i];
        do { tmp[tn++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
        while (tn > 0) buf[n++] = tmp[--tn];
        if (i < 2) buf[n++] = ';';
    }
    buf[n++] = 'm';
    write(1, buf, (size_t)n);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: color <text> | color <background> <text>\n");
        printf("Colors: black white red green blue yellow cyan magenta gray\n");
        printf("Hex: #RRGGBB or 0xRRGGBB\n");
        return 1;
    }

    unsigned fg = 0, bg = 0;
    int has_bg = (argc >= 3);

    if (has_bg) {
        if (!parse_color_token(argv[1], &bg)) { printf("color: invalid background color\n"); return 1; }
        if (!parse_color_token(argv[2], &fg)) { printf("color: invalid text color\n"); return 1; }
        emit_sgr(1, bg);
    } else {
        if (!parse_color_token(argv[1], &fg)) { printf("color: invalid text color\n"); return 1; }
    }
    emit_sgr(0, fg);

    printf(has_bg ? "Background and text colors updated\n" : "Text color updated\n");
    return 0;
}
