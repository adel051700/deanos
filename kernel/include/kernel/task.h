#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H

#include <stdint.h>
#include "signal.h"
#include "interrupt.h"
#include "syscall.h"

#define TASK_WAIT_NOHANG 0x1u
#define TASK_FD_CLOEXEC 0x1u
#define TASK_MAX_FDS    64
#define TASK_ELF_LAZY_MAX 16
#define TASK_MMAP_MAX   32
#define TASK_SHM_MAX_OBJECTS 16
#define TASK_SHM_MAX_PAGES   64

#define TASK_MMAP_BACKING_ANON 0u
#define TASK_MMAP_BACKING_FILE 1u
#define TASK_MMAP_BACKING_SHM  2u

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Configuration ----------------------------------------------------- */
#define TASK_MAX          32
#define TASK_NAME_LEN     16
#define TASK_DEFAULT_QUANTUM  5   /* PIT ticks per time-slice (50 ms at 100 Hz) */

/* ---- Types ------------------------------------------------------------- */

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

/* Only ESP is stored here; callee-saved regs live on the task's own stack. */
typedef struct task_context {
    uint32_t esp;
} task_context_t;

struct vfs_node;

typedef struct task_fd {
    struct vfs_node* node;
    uint32_t         offset;
    uint32_t         open_flags;
    uint32_t         fd_flags;
    uint8_t          in_use;
} task_fd_t;

typedef struct task_elf_lazy_region {
    uintptr_t start;
    uintptr_t end;
    uintptr_t file_start;
    uintptr_t file_end;
    uint32_t  file_offset;
    uint32_t  flags;
    uint8_t   in_use;
} task_elf_lazy_region_t;

typedef struct task_mmap_region {
    uintptr_t        start;
    uintptr_t        end;
    uint32_t         prot;
    uint32_t         flags;
    uint32_t         backing;
    uint32_t         file_offset;
    int32_t          shm_id;
    struct vfs_node* file_node;
    uint8_t          in_use;
} task_mmap_region_t;

typedef struct task {
    uint32_t        id;
    uint32_t        parent_id;
    task_state_t    state;
    char            name[TASK_NAME_LEN];

    task_context_t  ctx;

    /* Round-robin scheduling */
    uint32_t        quantum;        /* ticks allowed per slice           */
    uint32_t        ticks_left;     /* ticks remaining in current slice  */

    /* Blocking state */
    uint64_t        wake_tick;      /* scheduler tick to wake on (0 = none) */
    int             wait_task_id;   /* >0 specific child, -1 any child, 0 none */
    uint32_t        exit_status;    /* _exit(status) value */
    uint32_t        pending_signals;
    uint32_t        ignored_signals;
    uintptr_t       signal_handlers[KSIG_MAX + 1];
    uintptr_t       signal_restorers[KSIG_MAX + 1];
    uint8_t         signal_in_handler;
    uint32_t        signal_active;
    struct registers signal_saved_regs;
    uint32_t        term_signal;    /* terminating signal number (0 if normal exit) */
    uint8_t         wait_collected; /* set once parent reaps exit status */

    /* Kernel stack */
    uintptr_t       kstack_base;
    uint32_t        kstack_size;

    /* Per-process page-directory physical address (CR3 value). */
    uint32_t        mm_cr3;

    /* Session / job-control metadata. */
    uint32_t        sid;
    uint32_t        pgid;

    /* Security baseline credentials. */
    uint32_t        uid;
    uint32_t        gid;

    /* Per-process file descriptor table. */
    task_fd_t       fds[TASK_MAX_FDS];

    /* ELF-backed lazy user mappings (demand-loaded in page-fault handler). */
    uint8_t*        elf_backing;
    uint32_t        elf_backing_size;
    uint32_t        elf_region_count;
    task_elf_lazy_region_t elf_regions[TASK_ELF_LAZY_MAX];

    /* User mmap()/munmap() regions populated lazily by page-fault handler. */
    task_mmap_region_t mmap_regions[TASK_MMAP_MAX];

    /* Fork return context for child first run. */
    uint32_t        fork_user_eip;
    uint32_t        fork_user_esp;
    uint32_t        fork_user_eflags;
    uint8_t         fork_resume_user;
} task_t;

/* ---- Public API -------------------------------------------------------- */

void tasking_initialize(void);

/* Create a task.  quantum=0 → use TASK_DEFAULT_QUANTUM.  name may be NULL. */
int  task_create_named(void (*entry)(void), uint32_t stack_size,
                       uint32_t quantum, const char* name);

/* Convenience: default quantum, auto-generated name. */
int  task_create(void (*entry)(void), uint32_t stack_size);

void task_yield(void);
void task_exit(void);
void task_exit_with_status(uint32_t status);
/* Mark task DEAD by id. Returns 0 on success, negative on error. */
int  task_kill(int id);
int  task_send_signal(int pid, int sig);
int  task_send_signal_pgid(int pgid, int sig);
int  task_set_signal_ignored(int sig, int ignored);
/* Block until the task with the given ID is TASK_DEAD (or not found). */
void task_wait(int id);
int task_waitpid(int pid, int* status, uint32_t options);
/* Block current task for N scheduler ticks (N=0 => yield). */
void task_sleep_ticks(uint64_t ticks);
/* Convenience wrapper around scheduler ticks at 100 Hz PIT default. */
void task_sleep_ms(uint32_t milliseconds);

/* Called from PIT IRQ 0 (timer tick). */
void scheduler_tick(void);

/* Query helpers (for shell / diagnostics). */
uint32_t task_count(void);
const task_t* task_get(uint32_t index);
int task_current_id(void);
int task_current_ppid(void);
int task_parent_id(int id);
int task_current_sid(void);
int task_current_pgid(void);
int task_current_uid(void);
int task_current_gid(void);
int task_getsid(int pid);
int task_getpgid(int pid);
int task_setpgid(int pid, int pgid);
int task_setsid(void);
int task_pgid_exists_in_session(uint32_t sid, uint32_t pgid);
task_t* task_current(void);
void task_set_current_name(const char* name);

/* FD lifecycle helpers used by exec/vfs paths. */
void task_close_cloexec_fds_current(void);
int task_clone_fd_to_task(int task_id, int target_fd, int src_fd);
int task_assign_mm(int task_id, uint32_t mm_cr3);
int task_replace_current_mm(uint32_t mm_cr3);

/* Install/replace per-task ELF lazy mappings and backing image copy. */
int task_set_elf_lazy_layout(int task_id,
                             const uint8_t* image,
                             uint32_t image_size,
                             const task_elf_lazy_region_t* regions,
                             uint32_t region_count);

/* Same as task_set_elf_lazy_layout, but transfers image ownership to the task. */
int task_adopt_elf_lazy_layout(int task_id,
                               uint8_t* image,
                               uint32_t image_size,
                               const task_elf_lazy_region_t* regions,
                               uint32_t region_count);

/* mmap()/munmap() primitives for current task. */
int task_mmap_current(const syscall_mmap_args_t* args, uintptr_t* out_addr);
int task_munmap_current(uintptr_t addr, uint32_t length);
int task_shm_open_current(int32_t key, uint32_t size, uint32_t flags);
int task_shm_unlink_current(int32_t key);
int task_shm_get_frame(int32_t shm_id, uint32_t page_index, uintptr_t* out_frame, uint32_t* out_size);
int task_is_shared_page(uintptr_t page);

/* Fork groundwork: clone current task metadata and user return context. */
int task_fork_user(uint32_t user_eip, uint32_t user_esp, uint32_t user_eflags);

#ifdef __cplusplus
}
#endif

#endif