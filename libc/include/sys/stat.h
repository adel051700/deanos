#ifndef _SYS_STAT_H
#define _SYS_STAT_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct stat {
    uint32_t inode;
    uint32_t type;
    uint32_t size;
};
int fstat(int fd, struct stat* st);
#ifdef __cplusplus
}
#endif
#endif /* _SYS_STAT_H */
