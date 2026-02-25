#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct context {
    uint32_t ebx, esi, edi, ebp;
    uint32_t esp;
    uint32_t eip;
};

void context_switch(struct context* old_ctx, struct context* new_ctx);

#ifdef __cplusplus
}
#endif