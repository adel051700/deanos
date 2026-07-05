#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static void write_str(int fd, const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    write(fd, s, len);
}

static void write_int(int fd, int v) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    int neg = v < 0;
    unsigned uv = neg ? (unsigned)(-(v + 1)) + 1u : (unsigned)v;
    do {
        buf[--i] = (char)('0' + (uv % 10u));
        uv /= 10u;
    } while (uv != 0u);
    if (neg) buf[--i] = '-';
    write_str(fd, &buf[i]);
}

int main(void) {
    int fd = open("/fstest.out", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 1;

    char cwd_buf[256];
    char* cw = getcwd(cwd_buf, sizeof(cwd_buf));
    write_str(fd, "cwd_before=");
    write_str(fd, cw ? cw : "(getcwd failed)");
    write_str(fd, "\n");

    mkdir("/fstest_dir");
    int chdir_rc = chdir("/fstest_dir");
    write_str(fd, "chdir_rc=");
    write_int(fd, chdir_rc);
    write_str(fd, "\n");

    cw = getcwd(cwd_buf, sizeof(cwd_buf));
    write_str(fd, "cwd_after=");
    write_str(fd, cw ? cw : "(getcwd failed)");
    write_str(fd, "\n");

    /* Create a scratch file relative to the new cwd, then list the directory. */
    int scratch_fd = open("scratch.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (scratch_fd >= 0) {
        write_str(scratch_fd, "test data");
        close(scratch_fd);
    }

    unsigned idx = 0;
    unsigned entry_count = 0;
    struct dirent de;
    while (dir_read(".", idx, &de) == 0) {
        write_str(fd, "entry[");
        write_int(fd, (int)idx);
        write_str(fd, "]=");
        write_str(fd, de.name);
        write_str(fd, "\n");
        idx++;
        entry_count++;
    }
    write_str(fd, "entry_count=");
    write_int(fd, (int)entry_count);
    write_str(fd, "\n");

    struct stat st;
    int stat_rc = stat("scratch.txt", &st);
    write_str(fd, "stat_rc=");
    write_int(fd, stat_rc);
    write_str(fd, "\n");
    write_str(fd, "stat_size=");
    write_int(fd, (int)st.size);
    write_str(fd, "\n");

    int unlink_rc = unlink("scratch.txt");
    write_str(fd, "unlink_rc=");
    write_int(fd, unlink_rc);
    write_str(fd, "\n");

    int stat_after_unlink_rc = stat("scratch.txt", &st);
    write_str(fd, "stat_after_unlink_rc=");
    write_int(fd, stat_after_unlink_rc);
    write_str(fd, "\n");

    close(fd);
    return 0;
}
