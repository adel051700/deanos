#ifndef _DIRENT_H
#define _DIRENT_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
    char name[64];
    uint32_t inode;
    uint32_t type;
};

#define DT_FILE 0x01
#define DT_DIR  0x02

int dir_read(const char* path, unsigned index, struct dirent* out);

#ifdef __cplusplus
}
#endif
#endif /* _DIRENT_H */
