/*
 * task.c — Round-robin preemptive scheduler
 *
 * Each task gets a configurable time-slice (quantum) measured in PIT ticks.
 * On every PIT tick the running task's `ticks_left` is decremented.
 * A context switch happens only when:
 *   1. The quantum expires (ticks_left reaches 0), OR
 *   2. The task voluntarily yields (task_yield / task_exit).
 *
 * The idle task (index 0) is only chosen when nothing else is READY.
 */

#include "include/kernel/task.h"
#include "include/kernel/signal.h"
#include "include/kernel/kheap.h"
#include "include/kernel/paging.h"
#include "include/kernel/pmm.h"
#include "include/kernel/tss.h"
#include "include/kernel/usermode.h"
#include "include/kernel/vfs.h"
#include "include/kernel/blockdev.h"
#include "../libc/include/string.h"
#include <stddef.h>
#include <stdint.h>

#define DEFAULT_STACK_SIZE (16u * 1024u)
#define TASK_MMAP_BASE  0x90000000u
#define TASK_MMAP_LIMIT 0xB0000000u

extern void context_switch(task_context_t* old, task_context_t* next);
extern void task_trampoline(void);   /* in context_switch.s */

static task_t  g_tasks[TASK_MAX];
static uint32_t g_task_count = 0;
static int      g_current    = -1;
static uint32_t g_next_id    = 1;
static uint64_t g_sched_ticks = 0;

typedef struct shm_object {
    int32_t   key;
    uint32_t  size;
    uint32_t  page_count;
    uint32_t  refs;
    uint8_t   used;
    uint8_t   unlinked;
    uintptr_t frames[TASK_SHM_MAX_PAGES];
} shm_object_t;

static shm_object_t g_shm_objects[TASK_SHM_MAX_OBJECTS];

static void shm_table_init(void) {
    memset(g_shm_objects, 0, sizeof(g_shm_objects));
}

static shm_object_t* shm_find_by_key(int32_t key) {
    if (key <= 0) return NULL;
    for (uint32_t i = 0; i < TASK_SHM_MAX_OBJECTS; ++i) {
        shm_object_t* obj = &g_shm_objects[i];
        if (!obj->used || obj->unlinked) continue;
        if (obj->key == key) return obj;
    }
    return NULL;
}

static shm_object_t* shm_find_by_id(int32_t shm_id) {
    if (shm_id <= 0) return NULL;
    uint32_t idx = (uint32_t)(shm_id - 1);
    if (idx >= TASK_SHM_MAX_OBJECTS) return NULL;
    shm_object_t* obj = &g_shm_objects[idx];
    if (!obj->used) return NULL;
    return obj;
}

static int32_t shm_alloc_slot(void) {
    for (uint32_t i = 0; i < TASK_SHM_MAX_OBJECTS; ++i) {
        if (!g_shm_objects[i].used) return (int32_t)i;
    }
    return -1;
}

static void shm_destroy(shm_object_t* obj) {
    if (!obj || !obj->used) return;
    for (uint32_t i = 0; i < obj->page_count && i < TASK_SHM_MAX_PAGES; ++i) {
        if (obj->frames[i]) {
            phys_free_frame(obj->frames[i]);
            obj->frames[i] = 0;
        }
    }
    memset(obj, 0, sizeof(*obj));
}

static void shm_ref(int32_t shm_id) {
    shm_object_t* obj = shm_find_by_id(shm_id);
    if (!obj) return;
    obj->refs++;
}

static void shm_unref(int32_t shm_id) {
    shm_object_t* obj = shm_find_by_id(shm_id);
    if (!obj) return;
    if (obj->refs > 0) obj->refs--;
    if (obj->refs == 0 && obj->unlinked) {
        shm_destroy(obj);
    }
}

static void task_signal_table_init(task_t* t) {
    if (!t) return;
    for (uint32_t i = 0; i <= KSIG_MAX; ++i) {
        t->signal_handlers[i] = KSIG_DFL;
        t->signal_restorers[i] = 0;
    }
}

static void task_fd_table_init(task_t* t) {
    if (!t) return;
    for (int i = 0; i < TASK_MAX_FDS; ++i) {
        t->fds[i].node = NULL;
        t->fds[i].offset = 0;
        t->fds[i].open_flags = 0;
        t->fds[i].fd_flags = 0;
        t->fds[i].in_use = 0;
    }
}

static void task_fd_table_close_all(task_t* t) {
    if (!t) return;
    for (int i = 0; i < TASK_MAX_FDS; ++i) {
        if (!t->fds[i].in_use) continue;
        vfs_close_node(t->fds[i].node);
        t->fds[i].node = NULL;
        t->fds[i].offset = 0;
        t->fds[i].open_flags = 0;
        t->fds[i].fd_flags = 0;
        t->fds[i].in_use = 0;
    }
}

static int task_fd_table_clone(task_t* dst, const task_t* src) {
    if (!dst || !src) return -1;
    task_fd_table_init(dst);

    for (int i = 0; i < TASK_MAX_FDS; ++i) {
        if (!src->fds[i].in_use) continue;

        dst->fds[i] = src->fds[i];
        if (vfs_open_node(dst->fds[i].node, dst->fds[i].open_flags) < 0) {
            task_fd_table_close_all(dst);
            return -1;
        }
    }

    return 0;
}

static void task_fd_table_close_cloexec(task_t* t) {
    if (!t) return;
    for (int i = 0; i < TASK_MAX_FDS; ++i) {
        if (!t->fds[i].in_use) continue;
        if (!(t->fds[i].fd_flags & TASK_FD_CLOEXEC)) continue;
        vfs_close_node(t->fds[i].node);
        t->fds[i].node = NULL;
        t->fds[i].offset = 0;
        t->fds[i].open_flags = 0;
        t->fds[i].fd_flags = 0;
        t->fds[i].in_use = 0;
    }
}

static void task_mmap_table_init(task_t* t) {
    if (!t) return;
    for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
        t->mmap_regions[i].start = 0;
        t->mmap_regions[i].end = 0;
        t->mmap_regions[i].prot = 0;
        t->mmap_regions[i].flags = 0;
        t->mmap_regions[i].backing = TASK_MMAP_BACKING_ANON;
        t->mmap_regions[i].file_offset = 0;
        t->mmap_regions[i].shm_id = 0;
        t->mmap_regions[i].file_node = NULL;
        t->mmap_regions[i].in_use = 0;
    }
}

static void task_mmap_table_clear(task_t* t) {
    if (!t) return;
    for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
        task_mmap_region_t* reg = &t->mmap_regions[i];
        if (!reg->in_use) continue;
        if (reg->file_node) {
            vfs_close_node(reg->file_node);
        }
        if (reg->backing == TASK_MMAP_BACKING_SHM && reg->shm_id > 0) {
            shm_unref(reg->shm_id);
        }
        reg->start = 0;
        reg->end = 0;
        reg->prot = 0;
        reg->flags = 0;
        reg->backing = TASK_MMAP_BACKING_ANON;
        reg->file_offset = 0;
        reg->shm_id = 0;
        reg->file_node = NULL;
        reg->in_use = 0;
    }
}

static int task_mmap_table_clone(task_t* dst, const task_t* src) {
    if (!dst || !src) return -1;
    task_mmap_table_init(dst);

    for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
        const task_mmap_region_t* src_reg = &src->mmap_regions[i];
        if (!src_reg->in_use) continue;

        dst->mmap_regions[i] = *src_reg;
        if (dst->mmap_regions[i].file_node) {
            if (vfs_open_node(dst->mmap_regions[i].file_node, VFS_O_RDONLY) < 0) {
                task_mmap_table_clear(dst);
                return -1;
            }
        }
        if (dst->mmap_regions[i].backing == TASK_MMAP_BACKING_SHM && dst->mmap_regions[i].shm_id > 0) {
            shm_ref(dst->mmap_regions[i].shm_id);
        }
    }

    return 0;
}

static int task_mmap_alloc_slot(task_t* t) {
    if (!t) return -1;
    for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
        if (!t->mmap_regions[i].in_use) return (int)i;
    }
    return -1;
}

static uintptr_t page_align_down(uintptr_t v) {
    return v & ~0xFFFu;
}

static uintptr_t page_align_up(uintptr_t v) {
    return (v + 0xFFFu) & ~0xFFFu;
}

static int task_mmap_find_gap(task_t* t, uintptr_t length, uintptr_t* out_addr) {
    if (!t || !out_addr || length == 0) return -1;
    uintptr_t cur = TASK_MMAP_BASE;

    while (1) {
        uintptr_t end = cur + length;
        if (end < cur || end > TASK_MMAP_LIMIT) return -1;

        int overlap = 0;
        for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
            const task_mmap_region_t* reg = &t->mmap_regions[i];
            if (!reg->in_use) continue;
            if (end <= reg->start || cur >= reg->end) continue;
            cur = page_align_up(reg->end);
            overlap = 1;
            break;
        }

        if (!overlap) {
            *out_addr = cur;
            return 0;
        }
    }
}

/* ---- internal helpers -------------------------------------------------- */

static uint32_t init_task_id(void);
static int find_task_index_by_id(int id);
static void reparent_children(uint32_t old_parent_id, uint32_t new_parent_id);
static void release_task_mm(task_t* t);

static void task_lazy_elf_init(task_t* t) {
    if (!t) return;
    t->elf_backing = NULL;
    t->elf_backing_size = 0;
    t->elf_region_count = 0;
    for (uint32_t i = 0; i < TASK_ELF_LAZY_MAX; ++i) {
        t->elf_regions[i].start = 0;
        t->elf_regions[i].end = 0;
        t->elf_regions[i].file_start = 0;
        t->elf_regions[i].file_end = 0;
        t->elf_regions[i].file_offset = 0;
        t->elf_regions[i].flags = 0;
        t->elf_regions[i].in_use = 0;
    }
}

static void task_lazy_elf_clear(task_t* t) {
    if (!t) return;
    if (t->elf_backing) {
        kfree(t->elf_backing);
        t->elf_backing = NULL;
    }
    t->elf_backing_size = 0;
    t->elf_region_count = 0;
    for (uint32_t i = 0; i < TASK_ELF_LAZY_MAX; ++i) {
        t->elf_regions[i].in_use = 0;
    }
}

static int task_lazy_elf_install(task_t* t,
                                 const uint8_t* image,
                                 uint32_t image_size,
                                 const task_elf_lazy_region_t* regions,
                                 uint32_t region_count,
                                 int adopt_image) {
    if (!t) return -1;
    if (!regions && region_count > 0) return -1;
    if (region_count > TASK_ELF_LAZY_MAX) return -1;
    if (image_size > 0 && !image) return -1;

    uint8_t* backing_copy = NULL;
    if (image_size > 0) {
        if (adopt_image) {
            backing_copy = (uint8_t*)image;
        } else {
            backing_copy = (uint8_t*)kmalloc(image_size);
            if (!backing_copy) return -1;
            memcpy(backing_copy, image, image_size);
        }
    }

    task_lazy_elf_clear(t);
    t->elf_backing = backing_copy;
    t->elf_backing_size = image_size;
    t->elf_region_count = region_count;

    for (uint32_t i = 0; i < TASK_ELF_LAZY_MAX; ++i) {
        t->elf_regions[i].in_use = 0;
    }
    for (uint32_t i = 0; i < region_count; ++i) {
        t->elf_regions[i] = regions[i];
        t->elf_regions[i].in_use = 1;
    }
    return 0;
}

static void idle_thread(void) {
    for (;;)
        __asm__ __volatile__("hlt");
}

static void fork_child_entry(void) {
    if (g_current < 0) {
        task_exit();
        return;
    }

    task_t* self = &g_tasks[g_current];
    if (!self->fork_resume_user) {
        task_exit();
        return;
    }

    self->fork_resume_user = 0;
    enter_usermode_with_ret(self->fork_user_eip,
                            self->fork_user_esp,
                            self->fork_user_eflags,
                            0);

    task_exit();
}

/*
 * pick_next_ready — Round-robin selection.
 * Scans from (g_current+1) wrapping around.  Skips the idle task (index 0)
 * unless it is the only READY task.
 */
static int pick_next_ready(void) {
    if (g_task_count == 0) return -1;

    int start = (g_current < 0) ? 0 : g_current;
    int best  = -1;

    for (uint32_t n = 0; n < g_task_count; ++n) {
        int i = (start + 1 + (int)n) % (int)g_task_count;
        if (g_tasks[i].state != TASK_READY) continue;

        /* Prefer non-idle tasks; fall back to idle only if nothing else. */
        if (i == 0) {
            if (best < 0) best = 0;       /* remember idle as fallback */
        } else {
            return i;                      /* first non-idle READY task */
        }
    }
    return best;  /* either idle (0) or -1 if nothing ready */
}

static void* alloc_kstack(uint32_t size) {
    if (size == 0) size = DEFAULT_STACK_SIZE;
    return kmalloc(size);
}

static int is_task_dead_or_missing(int id) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        if ((int)g_tasks[i].id == id)
            return (g_tasks[i].state == TASK_DEAD);
    }
    return 1;
}

static int has_child_for_wait(uint32_t parent_id, int pid_filter) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        const task_t* t = &g_tasks[i];
        if (t->parent_id != parent_id) continue;
        if (pid_filter > 0 && (int)t->id != pid_filter) continue;
        if (t->state == TASK_DEAD && t->wait_collected) continue;
        return 1;
    }
    return 0;
}

static int find_waitable_child_index(uint32_t parent_id, int pid_filter) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        const task_t* t = &g_tasks[i];
        if (t->parent_id != parent_id) continue;
        if (pid_filter > 0 && (int)t->id != pid_filter) continue;
        if (t->state != TASK_DEAD) continue;
        if (t->wait_collected) continue;
        return (int)i;
    }
    return -1;
}

static void notify_parent_sigchld(uint32_t parent_id) {
    int parent_idx = find_task_index_by_id((int)parent_id);
    if (parent_idx < 0) return;
    g_tasks[parent_idx].pending_signals |= KSIG_BIT(KSIGCHLD);
}

static void mark_task_dead_index(uint32_t idx, uint32_t status, uint32_t term_sig) {
    if (idx >= g_task_count) return;
    if (g_tasks[idx].state == TASK_DEAD) return;

    uint32_t dying_id = g_tasks[idx].id;
    uint32_t parent_id = g_tasks[idx].parent_id;
    reparent_children(dying_id, init_task_id());
    task_fd_table_close_all(&g_tasks[idx]);
    task_mmap_table_clear(&g_tasks[idx]);
    task_lazy_elf_clear(&g_tasks[idx]);
    release_task_mm(&g_tasks[idx]);

    g_tasks[idx].wake_tick = 0;
    g_tasks[idx].wait_task_id = 0;
    g_tasks[idx].exit_status = status;
    g_tasks[idx].term_signal = term_sig;
    g_tasks[idx].pending_signals = 0;
    g_tasks[idx].signal_in_handler = 0;
    g_tasks[idx].signal_active = 0;
    g_tasks[idx].wait_collected = 0;
    g_tasks[idx].state = TASK_DEAD;

    notify_parent_sigchld(parent_id);
}

static void reap_task_index(uint32_t idx) {
    if (idx >= g_task_count) return;
    if ((int)idx == g_current) return;
    if (g_tasks[idx].state != TASK_DEAD) return;

    if (g_tasks[idx].kstack_base) {
        kfree((void*)g_tasks[idx].kstack_base);
        g_tasks[idx].kstack_base = 0;
    }

    for (uint32_t i = idx + 1; i < g_task_count; ++i) {
        g_tasks[i - 1] = g_tasks[i];
    }

    if (g_task_count > 0) g_task_count--;
    if (g_current > (int)idx) g_current--;
}

static void sweep_reapable_tasks(void) {
    uint32_t init_id = init_task_id();
    for (uint32_t i = 0; i < g_task_count; ++i) {
        task_t* t = &g_tasks[i];
        if (t->state != TASK_DEAD) continue;
        if (t->id == init_id) continue;

        /* Parent-reaped tasks and init-orphans are reclaimable. */
        if (t->wait_collected || t->parent_id == init_id) {
            reap_task_index(i);
            i--;
        }
    }
}

static int find_task_index_by_id(int id) {
    if (id <= 0) return -1;
    for (uint32_t i = 0; i < g_task_count; ++i) {
        if ((int)g_tasks[i].id == id) return (int)i;
    }
    return -1;
}

static int resolve_target_task_index(int pid) {
    if (g_current < 0) return -1;
    if (pid == 0) return g_current;
    return find_task_index_by_id(pid);
}

static int signal_is_fatal_default(int sig) {
    return (sig == KSIGINT || sig == KSIGTERM || sig == KSIGKILL);
}

static uint32_t init_task_id(void) {
    if (g_task_count > 0) return g_tasks[0].id;
    return 1;
}

static void reparent_children(uint32_t old_parent_id, uint32_t new_parent_id) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].parent_id == old_parent_id)
            g_tasks[i].parent_id = new_parent_id;
    }
}

static void release_task_mm(task_t* t) {
    if (!t) return;
    paging_release_mm(t->mm_cr3);
    t->mm_cr3 = 0;
}

static void wake_blocked_tasks(void) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        task_t* t = &g_tasks[i];
        if (t->state != TASK_BLOCKED) continue;

        if (t->wait_task_id > 0 && is_task_dead_or_missing(t->wait_task_id)) {
            int idx = find_task_index_by_id(t->wait_task_id);
            if (idx < 0 || (g_tasks[idx].state == TASK_DEAD && !g_tasks[idx].wait_collected)) {
                t->wait_task_id = 0;
                t->state = TASK_READY;
                continue;
            }
        }

        if (t->wait_task_id == -1) {
            int waitable = find_waitable_child_index(t->id, -1);
            if (waitable >= 0) {
                t->wait_task_id = 0;
                t->state = TASK_READY;
                continue;
            }
        }

        if (t->wake_tick != 0 && g_sched_ticks >= t->wake_tick) {
            t->wake_tick = 0;
            t->state = TASK_READY;
        }
    }
}

static void do_switch(int prev, int next) {
    if (prev >= 0 && g_tasks[prev].state == TASK_RUNNING)
        g_tasks[prev].state = TASK_READY;

    g_tasks[next].state      = TASK_RUNNING;
    g_tasks[next].ticks_left = g_tasks[next].quantum;

    /* Update TSS so ring-3 → ring-0 transitions use this task's kernel stack. */
    uint32_t kstack_top = g_tasks[next].kstack_base + g_tasks[next].kstack_size;
    tss_set_kernel_stack(kstack_top);

    paging_switch_mm(g_tasks[next].mm_cr3);

    g_current = next;

    context_switch(&g_tasks[prev].ctx, &g_tasks[next].ctx);
}

/* ---- public API -------------------------------------------------------- */

void tasking_initialize(void) {
    g_task_count = 0;
    g_current    = -1;
    g_next_id    = 1;
    g_sched_ticks = 0;
    shm_table_init();

    /* Task 0 is the idle thread — always present, lowest priority. */
    task_create_named(idle_thread, DEFAULT_STACK_SIZE, 1, "idle");

    g_current = 0;
    g_tasks[0].state = TASK_RUNNING;
}

int task_create_named(void (*entry)(void), uint32_t stack_size,
                      uint32_t quantum, const char* name)
{
    if (!entry) return -1;
    if (g_task_count >= TASK_MAX) {
        sweep_reapable_tasks();
        if (g_task_count >= TASK_MAX) return -2;
    }

    task_t* t = &g_tasks[g_task_count];
    t->id      = g_next_id++;
    t->parent_id = (g_current >= 0) ? g_tasks[g_current].id : 0;
    t->state   = TASK_READY;
    t->quantum = (quantum > 0) ? quantum : TASK_DEFAULT_QUANTUM;
    t->ticks_left = t->quantum;
    t->wake_tick = 0;
    t->wait_task_id = 0;
    t->exit_status = 0;
    t->pending_signals = 0;
    t->ignored_signals = 0;
    task_signal_table_init(t);
    t->signal_in_handler = 0;
    t->signal_active = 0;
    t->term_signal = 0;
    t->wait_collected = 0;
    if (g_current >= 0) {
        t->mm_cr3 = g_tasks[g_current].mm_cr3;
    } else {
        t->mm_cr3 = paging_current_cr3();
    }
    paging_retain_mm(t->mm_cr3);
    if (g_current >= 0) {
        t->sid = g_tasks[g_current].sid;
        t->pgid = g_tasks[g_current].pgid;
        t->uid = g_tasks[g_current].uid;
        t->gid = g_tasks[g_current].gid;
    } else {
        t->sid = t->id;
        t->pgid = t->id;
        t->uid = 0;
        t->gid = 0;
    }
    t->fork_user_eip = 0;
    t->fork_user_esp = 0;
    t->fork_user_eflags = 0;
    t->fork_resume_user = 0;
    task_fd_table_init(t);
    task_mmap_table_init(t);
    task_lazy_elf_init(t);

    /* Copy name (or generate one). */
    if (name) {
        uint32_t i = 0;
        for (; i < TASK_NAME_LEN - 1 && name[i]; ++i)
            t->name[i] = name[i];
        t->name[i] = '\0';
    } else {
        /* "task_<id>" */
        t->name[0] = 't'; t->name[1] = '_';
        /* simple decimal id */
        uint32_t id = t->id;
        char tmp[12];
        int len = 0;
        do { tmp[len++] = '0' + (id % 10); id /= 10; } while (id);
        uint32_t p = 2;
        for (int j = len - 1; j >= 0 && p < TASK_NAME_LEN - 1; --j)
            t->name[p++] = tmp[j];
        t->name[p] = '\0';
    }

    if (stack_size == 0) stack_size = DEFAULT_STACK_SIZE;
    void* stack = alloc_kstack(stack_size);
    if (!stack) return -3;

    t->kstack_base = (uintptr_t)stack;
    t->kstack_size = stack_size;

    /* Build initial stack frame for context_switch's pop/pop/pop/pop/ret */
    uintptr_t top = (uintptr_t)stack + stack_size;
    top &= ~0xFu;

    uint32_t* sp = (uint32_t*)top;
    *(--sp) = (uint32_t)task_trampoline;   /* return address         */
    *(--sp) = 0;                            /* ebp                    */
    *(--sp) = (uint32_t)entry;             /* ebx = entry point      */
    *(--sp) = 0;                            /* esi                    */
    *(--sp) = 0;                            /* edi                    */
    t->ctx.esp = (uint32_t)sp;

    g_task_count++;
    return (int)t->id;
}

int task_create(void (*entry)(void), uint32_t stack_size) {
    return task_create_named(entry, stack_size, TASK_DEFAULT_QUANTUM, NULL);
}

void task_exit(void) {
    task_exit_with_status(0);
}

void task_exit_with_status(uint32_t status) {
    if (g_current >= 0) {
        mark_task_dead_index((uint32_t)g_current, status, 0);
    }
    task_yield();
    for (;;) __asm__ __volatile__("hlt");
}

int task_kill(int id) {
    return task_send_signal(id, KSIGKILL);
}

int task_fork_user(uint32_t user_eip, uint32_t user_esp, uint32_t user_eflags) {
    if (g_current < 0) return -1;

    task_t* parent = &g_tasks[g_current];
    int child_id = task_create_named(fork_child_entry,
                                     parent->kstack_size,
                                     parent->quantum,
                                     parent->name);
    if (child_id < 0) return child_id;

    int child_idx = find_task_index_by_id(child_id);
    if (child_idx < 0) return -2;

    task_t* child = &g_tasks[child_idx];
    child->parent_id = parent->id;
    child->ignored_signals = parent->ignored_signals;
    for (uint32_t i = 0; i <= KSIG_MAX; ++i) {
        child->signal_handlers[i] = parent->signal_handlers[i];
        child->signal_restorers[i] = parent->signal_restorers[i];
    }

    uint32_t child_mm_cr3 = 0;
    if (paging_fork_current_cow(&child_mm_cr3) < 0) {
        child->state = TASK_DEAD;
        return -3;
    }

    release_task_mm(child);
    child->mm_cr3 = child_mm_cr3;
    if (task_fd_table_clone(child, parent) < 0) {
        release_task_mm(child);
        child->state = TASK_DEAD;
        return -4;
    }
    if (task_mmap_table_clone(child, parent) < 0) {
        task_fd_table_close_all(child);
        release_task_mm(child);
        child->state = TASK_DEAD;
        return -5;
    }
    if (task_lazy_elf_install(child,
                              parent->elf_backing,
                              parent->elf_backing_size,
                              parent->elf_regions,
                              parent->elf_region_count,
                              0) < 0) {
        task_mmap_table_clear(child);
        task_fd_table_close_all(child);
        release_task_mm(child);
        child->state = TASK_DEAD;
        return -6;
    }
    child->fork_user_eip = user_eip;
    child->fork_user_esp = user_esp;
    child->fork_user_eflags = user_eflags;
    child->fork_resume_user = 1;
    return child_id;
}

void task_wait(int id) {
    (void)task_waitpid(id, NULL, 0);
}

int task_waitpid(int pid, int* status, uint32_t options) {
    if (g_current < 0) return -1;
    if (pid == 0 || pid < -1) return -2;

    uint32_t parent_id = g_tasks[g_current].id;

    for (;;) {
        int child_idx = find_waitable_child_index(parent_id, pid);
        if (child_idx >= 0) {
            task_t* child = &g_tasks[child_idx];
            int child_id = (int)child->id;
            uint32_t child_status = child->exit_status;
            if (status) *status = (int)child->exit_status;
            child->wait_collected = 1;

            reap_task_index((uint32_t)child_idx);
            if (status) *status = (int)child_status;
            return child_id;
        }

        if (!has_child_for_wait(parent_id, pid)) return -3;
        if (options & TASK_WAIT_NOHANG) return 0;

        g_tasks[g_current].wait_task_id = (pid > 0) ? pid : -1;
        g_tasks[g_current].wake_tick = 0;
        g_tasks[g_current].state = TASK_BLOCKED;
        task_yield();
    }
}

int task_send_signal(int pid, int sig) {
    if (!signal_is_supported(sig)) return -1;

    int idx = find_task_index_by_id(pid);
    if (idx < 0) return -1;
    if (idx == 0 && signal_is_fatal_default(sig)) return -2; /* never kill idle */

    task_t* t = &g_tasks[idx];
    if (t->state == TASK_DEAD) return 0;

    uint32_t bit = KSIG_BIT(sig);
    t->pending_signals |= bit;

    uintptr_t disposition = KSIG_DFL;
    if (sig > 0 && sig <= KSIG_MAX) {
        disposition = t->signal_handlers[(uint32_t)sig];
    }

    if ((t->ignored_signals & bit) || disposition == KSIG_IGN) {
        t->pending_signals &= ~bit;
        return 0;
    }

    if (disposition > KSIG_IGN) {
        if (t->state == TASK_BLOCKED) {
            t->state = TASK_READY;
            t->wake_tick = 0;
            t->wait_task_id = 0;
        }
        return 0;
    }

    if (sig == KSIGCHLD) {
        return 0;
    }

    if (!signal_is_fatal_default(sig)) {
        return 0;
    }

    t->pending_signals &= ~bit;
    if (idx == g_current) {
        task_exit_with_status(signal_default_exit_status(sig));
        return 0;
    }

    mark_task_dead_index((uint32_t)idx, signal_default_exit_status(sig), (uint32_t)sig);
    return 0;
}

int task_send_signal_pgid(int pgid, int sig) {
    if (!signal_is_supported(sig)) return -1;
    if (pgid <= 0) return -1;

    int matched = 0;
    for (uint32_t i = 0; i < g_task_count; ++i) {
        task_t* t = &g_tasks[i];
        if (t->state == TASK_DEAD) continue;
        if ((int)t->pgid != pgid) continue;
        if ((int)i == 0 && signal_is_fatal_default(sig)) continue;
        matched = 1;
        (void)task_send_signal((int)t->id, sig);
    }

    return matched ? 0 : -1;
}

int task_set_signal_ignored(int sig, int ignored) {
    if (g_current < 0) return -1;
    if (!signal_is_supported(sig)) return -1;
    if (sig == KSIGKILL) return -1;

    uint32_t bit = KSIG_BIT(sig);
    if (ignored) {
        g_tasks[g_current].ignored_signals |= bit;
        if (sig > 0 && sig <= KSIG_MAX) {
            g_tasks[g_current].signal_handlers[(uint32_t)sig] = KSIG_IGN;
            g_tasks[g_current].signal_restorers[(uint32_t)sig] = 0;
        }
        g_tasks[g_current].pending_signals &= ~bit;
    } else {
        g_tasks[g_current].ignored_signals &= ~bit;
        if (sig > 0 && sig <= KSIG_MAX && g_tasks[g_current].signal_handlers[(uint32_t)sig] == KSIG_IGN) {
            g_tasks[g_current].signal_handlers[(uint32_t)sig] = KSIG_DFL;
            g_tasks[g_current].signal_restorers[(uint32_t)sig] = 0;
        }
    }
    return 0;
}

void task_sleep_ticks(uint64_t ticks) {
    if (ticks == 0) {
        task_yield();
        return;
    }
    if (g_current <= 0) return; /* never block idle/non-running context */

    g_tasks[g_current].wake_tick = g_sched_ticks + ticks;
    g_tasks[g_current].wait_task_id = 0;
    g_tasks[g_current].state = TASK_BLOCKED;
    task_yield();
}

void task_sleep_ms(uint32_t milliseconds) {
    /* Scheduler is PIT-driven at 100 Hz (10 ms per tick). */
    uint64_t ticks = ((uint64_t)milliseconds + 9u) / 10u;
    if (ticks == 0) ticks = 1;
    task_sleep_ticks(ticks);
}

void task_yield(void) {
    __asm__ __volatile__("int $0x20");
}

/*
 * scheduler_tick — called from PIT IRQ 0.
 *
 * Round-robin logic:
 *   • Decrement the running task's ticks_left.
 *   • If ticks_left > 0 AND the task is still RUNNING → keep running.
 *   • Otherwise pick the next READY task and switch.
 *   • If no other task is ready, let the current one keep going
 *     (or switch to idle if the current one died / blocked).
 */
void scheduler_tick(void) {
    g_sched_ticks++;
    blockdev_pump(1);
    wake_blocked_tasks();

    /* Decrement remaining slice of current task. */
    if (g_current >= 0 && g_tasks[g_current].state == TASK_RUNNING) {
        if (g_tasks[g_current].ticks_left > 0)
            g_tasks[g_current].ticks_left--;

        /* Still has time left → no switch. */
        if (g_tasks[g_current].ticks_left > 0)
            return;
    }

    /* Quantum expired or task is no longer RUNNING → find next. */
    int next = pick_next_ready();
    if (next < 0) return;                    /* nothing to run at all     */
    if (next == g_current) {
        /* Same task — just refill its quantum. */
        g_tasks[g_current].ticks_left = g_tasks[g_current].quantum;
        return;
    }

    int prev = g_current;
    do_switch(prev, next);
}

/* ---- Query helpers ----------------------------------------------------- */

uint32_t task_count(void)              { return g_task_count; }
const task_t* task_get(uint32_t idx)   { return (idx < g_task_count) ? &g_tasks[idx] : NULL; }
int task_current_id(void)              { return (g_current >= 0) ? (int)g_tasks[g_current].id : -1; }
int task_current_ppid(void)            { return (g_current >= 0) ? (int)g_tasks[g_current].parent_id : -1; }
int task_parent_id(int id) {
    int idx = find_task_index_by_id(id);
    if (idx < 0) return -1;
    return (int)g_tasks[idx].parent_id;
}

int task_current_sid(void) {
    return (g_current >= 0) ? (int)g_tasks[g_current].sid : -1;
}

int task_current_pgid(void) {
    return (g_current >= 0) ? (int)g_tasks[g_current].pgid : -1;
}

int task_current_uid(void) {
    return (g_current >= 0) ? (int)g_tasks[g_current].uid : -1;
}

int task_current_gid(void) {
    return (g_current >= 0) ? (int)g_tasks[g_current].gid : -1;
}

int task_getsid(int pid) {
    int idx = resolve_target_task_index(pid);
    if (idx < 0) return -1;
    return (int)g_tasks[idx].sid;
}

int task_getpgid(int pid) {
    int idx = resolve_target_task_index(pid);
    if (idx < 0) return -1;
    return (int)g_tasks[idx].pgid;
}

int task_pgid_exists_in_session(uint32_t sid, uint32_t pgid) {
    if (sid == 0 || pgid == 0) return 0;
    for (uint32_t i = 0; i < g_task_count; ++i) {
        const task_t* t = &g_tasks[i];
        if (t->state == TASK_DEAD) continue;
        if (t->sid == sid && t->pgid == pgid) return 1;
    }
    return 0;
}

int task_setpgid(int pid, int pgid) {
    if (g_current < 0) return -1;

    int target_idx = resolve_target_task_index(pid);
    if (target_idx < 0) return -1;

    task_t* caller = &g_tasks[g_current];
    task_t* target = &g_tasks[target_idx];
    if (target->state == TASK_DEAD) return -1;

    if (target_idx != g_current && target->parent_id != caller->id) {
        return -1;
    }

    if (pgid == 0) pgid = (int)target->id;
    if (pgid <= 0) return -1;

    if (target->sid != caller->sid) return -1;
    if ((uint32_t)pgid != target->id && !task_pgid_exists_in_session(target->sid, (uint32_t)pgid)) {
        return -1;
    }

    target->pgid = (uint32_t)pgid;
    return 0;
}

int task_setsid(void) {
    if (g_current < 0) return -1;
    task_t* self = &g_tasks[g_current];

    if (self->pgid == self->id) {
        return -1;
    }

    self->sid = self->id;
    self->pgid = self->id;
    return (int)self->sid;
}

task_t* task_current(void) {
    return (g_current >= 0) ? &g_tasks[g_current] : NULL;
}

void task_set_current_name(const char* name) {
    if (g_current < 0 || !name) return;
    task_t* t = &g_tasks[g_current];
    uint32_t i = 0;
    for (; i < TASK_NAME_LEN - 1 && name[i]; ++i) {
        t->name[i] = name[i];
    }
    t->name[i] = '\0';
}

void task_close_cloexec_fds_current(void) {
    if (g_current < 0) return;
    task_fd_table_close_cloexec(&g_tasks[g_current]);
}

int task_clone_fd_to_task(int task_id, int target_fd, int src_fd) {
    if (g_current < 0) return -1;
    if (task_id <= 0) return -1;
    if (target_fd < 0 || target_fd >= TASK_MAX_FDS) return -1;
    if (src_fd < 0 || src_fd >= TASK_MAX_FDS) return -1;

    task_t* src_task = &g_tasks[g_current];
    if (!src_task->fds[src_fd].in_use) return -1;

    int dst_idx = find_task_index_by_id(task_id);
    if (dst_idx < 0) return -1;
    task_t* dst_task = &g_tasks[dst_idx];

    if (dst_task->fds[target_fd].in_use) {
        vfs_close_node(dst_task->fds[target_fd].node);
        dst_task->fds[target_fd].node = NULL;
        dst_task->fds[target_fd].offset = 0;
        dst_task->fds[target_fd].open_flags = 0;
        dst_task->fds[target_fd].fd_flags = 0;
        dst_task->fds[target_fd].in_use = 0;
    }

    dst_task->fds[target_fd] = src_task->fds[src_fd];
    if (vfs_open_node(dst_task->fds[target_fd].node, dst_task->fds[target_fd].open_flags) < 0) {
        dst_task->fds[target_fd].node = NULL;
        dst_task->fds[target_fd].offset = 0;
        dst_task->fds[target_fd].open_flags = 0;
        dst_task->fds[target_fd].fd_flags = 0;
        dst_task->fds[target_fd].in_use = 0;
        return -1;
    }

    return 0;
}

int task_assign_mm(int task_id, uint32_t mm_cr3) {
    if (task_id <= 0 || !mm_cr3) return -1;
    int idx = find_task_index_by_id(task_id);
    if (idx < 0) return -1;

    task_t* t = &g_tasks[idx];
    paging_retain_mm(mm_cr3);
    paging_release_mm(t->mm_cr3);
    t->mm_cr3 = mm_cr3 & ~0xFFFu;
    return 0;
}

int task_replace_current_mm(uint32_t mm_cr3) {
    if (g_current < 0 || !mm_cr3) return -1;
    task_t* self = &g_tasks[g_current];
    uint32_t old = self->mm_cr3;
    task_mmap_table_clear(self);
    task_lazy_elf_clear(self);
    self->mm_cr3 = mm_cr3 & ~0xFFFu;
    paging_switch_mm(self->mm_cr3);
    paging_release_mm(old);
    return 0;
}

int task_set_elf_lazy_layout(int task_id,
                             const uint8_t* image,
                             uint32_t image_size,
                             const task_elf_lazy_region_t* regions,
                             uint32_t region_count) {
    if (task_id <= 0) return -1;
    int idx = find_task_index_by_id(task_id);
    if (idx < 0) return -1;
    return task_lazy_elf_install(&g_tasks[idx], image, image_size, regions, region_count, 0);
}

int task_adopt_elf_lazy_layout(int task_id,
                               uint8_t* image,
                               uint32_t image_size,
                               const task_elf_lazy_region_t* regions,
                               uint32_t region_count) {
    if (task_id <= 0) return -1;
    int idx = find_task_index_by_id(task_id);
    if (idx < 0) return -1;
    return task_lazy_elf_install(&g_tasks[idx], image, image_size, regions, region_count, 1);
}

int task_mmap_current(const syscall_mmap_args_t* args, uintptr_t* out_addr) {
    if (!args || !out_addr || g_current < 0) return -1;

    task_t* self = &g_tasks[g_current];
    uintptr_t length = page_align_up((uintptr_t)args->length);
    if (length == 0) return -1;
    if (args->flags & MMAP_MAP_FIXED) return -1;

    uint32_t map_kind = args->flags & (MMAP_MAP_PRIVATE | MMAP_MAP_SHARED);
    if (map_kind == 0 || map_kind == (MMAP_MAP_PRIVATE | MMAP_MAP_SHARED)) return -1;

    int slot = task_mmap_alloc_slot(self);
    if (slot < 0) return -1;

    uintptr_t start = 0;
    if (args->addr != 0) {
        start = page_align_down(args->addr);
        if (start < TASK_MMAP_BASE) return -1;
        if (start + length < start || start + length > TASK_MMAP_LIMIT) return -1;
        for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
            task_mmap_region_t* reg = &self->mmap_regions[i];
            if (!reg->in_use) continue;
            if (start + length <= reg->start || start >= reg->end) continue;
            return -1;
        }
    } else {
        if (task_mmap_find_gap(self, length, &start) < 0) return -1;
    }

    vfs_node_t* file_node = NULL;
    int32_t shm_id = 0;
    uint32_t backing = TASK_MMAP_BACKING_ANON;
    uint32_t file_offset = args->offset;

    if (args->flags & MMAP_MAP_SHM) {
        if (args->flags & MMAP_MAP_ANONYMOUS) return -1;
        if ((args->flags & MMAP_MAP_SHARED) == 0u) return -1;
        if ((args->offset & 0xFFFu) != 0u) return -1;
        if (args->fd <= 0) return -1;

        shm_object_t* obj = shm_find_by_id(args->fd);
        if (!obj || obj->unlinked) return -1;
        if (file_offset >= obj->size) return -1;
        if (length > (uintptr_t)(obj->size - file_offset)) return -1;

        shm_id = args->fd;
        backing = TASK_MMAP_BACKING_SHM;
        shm_ref(shm_id);
    } else if (!(args->flags & MMAP_MAP_ANONYMOUS)) {
        if ((args->offset & 0xFFFu) != 0u) return -1;
        if (args->fd < 0 || args->fd >= TASK_MAX_FDS) return -1;
        if (!self->fds[args->fd].in_use || !self->fds[args->fd].node) return -1;
        if (!(self->fds[args->fd].node->type & VFS_FILE)) return -1;
        file_node = self->fds[args->fd].node;
        if (vfs_open_node(file_node, VFS_O_RDONLY) < 0) return -1;
        backing = TASK_MMAP_BACKING_FILE;
    }

    task_mmap_region_t* reg = &self->mmap_regions[(uint32_t)slot];
    reg->start = start;
    reg->end = start + length;
    reg->prot = args->prot;
    reg->flags = args->flags;
    reg->backing = backing;
    reg->file_offset = file_offset;
    reg->shm_id = shm_id;
    reg->file_node = file_node;
    reg->in_use = 1;

    *out_addr = start;
    return 0;
}

int task_munmap_current(uintptr_t addr, uint32_t length) {
    if (g_current < 0) return -1;
    if (length == 0) return -1;

    task_t* self = &g_tasks[g_current];
    uintptr_t start = page_align_down(addr);
    uintptr_t end = page_align_up(addr + (uintptr_t)length);
    if (end <= start) return -1;

    int split_needed = 0;
    int free_slots = 0;
    int touched = 0;

    for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
        task_mmap_region_t* reg = &self->mmap_regions[i];
        if (!reg->in_use) {
            free_slots++;
            continue;
        }
        if (end <= reg->start || start >= reg->end) continue;
        touched = 1;
        if (start > reg->start && end < reg->end) split_needed++;
    }

    if (!touched) return -1;
    if (split_needed > free_slots) return -1;

    for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
        task_mmap_region_t* reg = &self->mmap_regions[i];
        if (!reg->in_use) continue;
        if (end <= reg->start || start >= reg->end) continue;

        uintptr_t overlap_start = (start > reg->start) ? start : reg->start;
        uintptr_t overlap_end = (end < reg->end) ? end : reg->end;
        if (overlap_end > overlap_start) {
            (void)paging_unmap_user_range(overlap_start, overlap_end - overlap_start);
        }

        if (start <= reg->start && end >= reg->end) {
            if (reg->file_node) vfs_close_node(reg->file_node);
            if (reg->backing == TASK_MMAP_BACKING_SHM && reg->shm_id > 0) shm_unref(reg->shm_id);
            reg->in_use = 0;
            reg->file_node = NULL;
            reg->shm_id = 0;
            continue;
        }

        if (start <= reg->start) {
            uint32_t delta = (uint32_t)(overlap_end - reg->start);
            reg->start = overlap_end;
            reg->file_offset += delta;
            continue;
        }

        if (end >= reg->end) {
            reg->end = overlap_start;
            continue;
        }

        int split_slot = task_mmap_alloc_slot(self);
        if (split_slot < 0) return -1;

        task_mmap_region_t* upper = &self->mmap_regions[(uint32_t)split_slot];
        *upper = *reg;
        upper->start = overlap_end;
        upper->file_offset += (uint32_t)(upper->start - reg->start);
        if (upper->file_node) {
            if (vfs_open_node(upper->file_node, VFS_O_RDONLY) < 0) {
                upper->in_use = 0;
                return -1;
            }
        }
        if (upper->backing == TASK_MMAP_BACKING_SHM && upper->shm_id > 0) {
            shm_ref(upper->shm_id);
        }
        reg->end = overlap_start;
    }

    return 0;
}

int task_shm_open_current(int32_t key, uint32_t size, uint32_t flags) {
    if (key <= 0) return -1;

    shm_object_t* existing = shm_find_by_key(key);
    if (existing) {
        if ((flags & SHM_OPEN_CREATE) && (flags & SHM_OPEN_EXCL)) return -1;
        if (size > 0 && size > existing->size) return -1;
        return (int)((existing - g_shm_objects) + 1);
    }

    if ((flags & SHM_OPEN_CREATE) == 0u) return -1;
    if (size == 0) return -1;
    uint32_t aligned = (size + 0xFFFu) & ~0xFFFu;
    uint32_t pages = aligned / 0x1000u;
    if (pages == 0 || pages > TASK_SHM_MAX_PAGES) return -1;

    int32_t slot = shm_alloc_slot();
    if (slot < 0) return -1;

    shm_object_t* obj = &g_shm_objects[(uint32_t)slot];
    memset(obj, 0, sizeof(*obj));
    obj->used = 1;
    obj->key = key;
    obj->size = aligned;
    obj->page_count = pages;
    obj->refs = 0;
    obj->unlinked = 0;

    for (uint32_t i = 0; i < pages; ++i) {
        uintptr_t pa = phys_alloc_frame();
        if (!pa) {
            shm_destroy(obj);
            return -1;
        }
        memset((void*)pa, 0, 4096u);
        obj->frames[i] = pa;
    }

    return slot + 1;
}

int task_shm_unlink_current(int32_t key) {
    shm_object_t* obj = shm_find_by_key(key);
    if (!obj) return -1;
    obj->unlinked = 1;
    if (obj->refs == 0) {
        shm_destroy(obj);
    }
    return 0;
}

int task_shm_get_frame(int32_t shm_id, uint32_t page_index, uintptr_t* out_frame, uint32_t* out_size) {
    if (!out_frame) return -1;
    shm_object_t* obj = shm_find_by_id(shm_id);
    if (!obj) return -1;
    if (page_index >= obj->page_count) return -1;
    if (!obj->frames[page_index]) return -1;
    *out_frame = obj->frames[page_index];
    if (out_size) *out_size = obj->size;
    return 0;
}

int task_is_shared_page(uintptr_t page) {
    task_t* self = task_current();
    if (!self) return 0;
    uintptr_t aligned = page_align_down(page);
    for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
        const task_mmap_region_t* reg = &self->mmap_regions[i];
        if (!reg->in_use) continue;
        if (reg->backing != TASK_MMAP_BACKING_SHM) continue;
        if ((reg->flags & MMAP_MAP_SHARED) == 0u) continue;
        if (aligned >= reg->start && aligned < reg->end) return 1;
    }
    return 0;
}

