#ifndef _SYS_PROC_H
#define _SYS_PROC_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct task_info {
    int32_t  id;
    int32_t  parent_id;
    int32_t  sid;
    int32_t  pgid;
    int32_t  state;
    uint32_t quantum;
    char     name[16];
};

int task_list(unsigned index, struct task_info* out);

#ifdef __cplusplus
}
#endif
#endif /* _SYS_PROC_H */
