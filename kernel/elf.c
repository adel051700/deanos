/*
 * elf.c — Minimal ELF32 loader for DeanOS
 */
#include "include/kernel/elf.h"
#include "include/kernel/vfs.h"
#include "include/kernel/kheap.h"
#include "include/kernel/paging.h"
#include "include/kernel/task.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/usermode.h"
#include <string.h>
#define ELF_USER_STACK_SIZE  (8u * 1024u)

static const char* path_basename(const char* path) {
    if (!path) return NULL;
    const char* name = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' && *(p + 1) != '\0') name = p + 1;
    }
    return name;
}

static int elf_read_from_vfs(const char* path, uint8_t** out_buf, uint32_t* out_size) {
    if (!path || !out_buf || !out_size) return -1;
    vfs_node_t* node = vfs_namei(path);
    if (!node)                     return -1;
    if (!(node->type & VFS_FILE)) return -2;
    if (node->size == 0)           return -3;

    uint8_t* buf = (uint8_t*)kmalloc(node->size);
    if (!buf) return -4;

    int32_t nread = vfs_read(node, 0, node->size, buf);
    if (nread <= 0) {
        kfree(buf);
        return -5;
    }

    *out_buf = buf;
    *out_size = (uint32_t)nread;
    return 0;
}
int elf_validate(const uint8_t* data, uint32_t size) {
    if (size < sizeof(Elf32_Ehdr)) return -1;
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)data;
    if (eh->e_ident[EI_MAG0] != 0x7f || eh->e_ident[EI_MAG1] != 'E' ||
        eh->e_ident[EI_MAG2] != 'L'  || eh->e_ident[EI_MAG3] != 'F')
        return -2;
    if (eh->e_ident[EI_CLASS] != ELFCLASS32)  return -3;
    if (eh->e_ident[EI_DATA]  != ELFDATA2LSB) return -4;
    if (eh->e_type   != ET_EXEC)              return -5;
    if (eh->e_machine != EM_386)              return -6;
    if (eh->e_phnum  == 0)                    return -7;
    return 0;
}
static int elf_load_segments(const uint8_t* data, uint32_t size,
                             const Elf32_Ehdr* eh)
{
    uint32_t ph_off = eh->e_phoff;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph_off + sizeof(Elf32_Phdr) > size) return -1;
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(data + ph_off);
        if (ph->p_type == PT_LOAD && ph->p_memsz > 0) {
            uintptr_t seg_start = ph->p_vaddr & ~0xFFFu;
            uintptr_t seg_end   = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFu;
            for (uintptr_t va = seg_start; va < seg_end; va += 4096) {
                if (paging_map_user(va) < 0) return -2;
            }
            if (ph->p_filesz > 0) {
                if (ph->p_offset + ph->p_filesz > size) return -3;
                memcpy((void*)(uintptr_t)ph->p_vaddr,
                       data + ph->p_offset, ph->p_filesz);
            }
            if (ph->p_memsz > ph->p_filesz) {
                memset((void*)(uintptr_t)(ph->p_vaddr + ph->p_filesz),
                       0, ph->p_memsz - ph->p_filesz);
            }
        }
        ph_off += eh->e_phentsize;
    }
    return 0;
}
typedef struct {
    int       in_use;
    int       task_id;
    uint32_t  entry;
    uintptr_t ustk;
} elf_launch_slot_t;

static elf_launch_slot_t g_elf_launch_slots[TASK_MAX];

static int elf_launch_slot_set(int task_id, uint32_t entry, uintptr_t ustk) {
    for (int i = 0; i < TASK_MAX; ++i) {
        if (g_elf_launch_slots[i].in_use) continue;
        g_elf_launch_slots[i].in_use = 1;
        g_elf_launch_slots[i].task_id = task_id;
        g_elf_launch_slots[i].entry = entry;
        g_elf_launch_slots[i].ustk = ustk;
        return 0;
    }
    return -1;
}

static elf_launch_slot_t* elf_launch_slot_take(int task_id) {
    for (int i = 0; i < TASK_MAX; ++i) {
        if (!g_elf_launch_slots[i].in_use) continue;
        if (g_elf_launch_slots[i].task_id != task_id) continue;

        g_elf_launch_slots[i].in_use = 0;
        return &g_elf_launch_slots[i];
    }
    return NULL;
}

static void elf_launch_slot_clear(int task_id) {
    for (int i = 0; i < TASK_MAX; ++i) {
        if (!g_elf_launch_slots[i].in_use) continue;
        if (g_elf_launch_slots[i].task_id != task_id) continue;
        g_elf_launch_slots[i].in_use = 0;
        return;
    }
}
extern const uint8_t _binary_build_user_anim_elf_start[];
extern const uint8_t _binary_build_user_anim_elf_end[];
extern const uint8_t _binary_build_user_forktest_elf_start[];
extern const uint8_t _binary_build_user_forktest_elf_end[];
extern const uint8_t _binary_build_user_execvetest_elf_start[];
extern const uint8_t _binary_build_user_execvetest_elf_end[];
extern const uint8_t _binary_build_user_waittest_elf_start[];
extern const uint8_t _binary_build_user_waittest_elf_end[];
extern const uint8_t _binary_build_user_waitstress_elf_start[];
extern const uint8_t _binary_build_user_waitstress_elf_end[];
extern const uint8_t _binary_build_user_waitstressbg_elf_start[];
extern const uint8_t _binary_build_user_waitstressbg_elf_end[];
extern const uint8_t _binary_build_user_catfd_elf_start[];
extern const uint8_t _binary_build_user_catfd_elf_end[];

static void elf_task_wrapper(void) {
    int tid = task_current_id();
    elf_launch_slot_t* slot = elf_launch_slot_take(tid);
    if (!slot) {
        task_exit();
        return;
    }

    uint32_t  entry    = slot->entry;
    uintptr_t ustk     = slot->ustk;
    uint32_t  user_esp = (uint32_t)(ustk + ELF_USER_STACK_SIZE) & ~0xFu;
    enter_usermode(entry, user_esp);
}

int elf_exec_with_stdio(const char* path, int wait, int stdin_fd, int stdout_fd) {
    uint8_t* buf = NULL;
    uint32_t size = 0;
    int io = elf_read_from_vfs(path, &buf, &size);
    if (io < 0) return io;

    int err = elf_validate(buf, size);
    if (err < 0) { kfree(buf); return err - 10; }
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)buf;
    if (elf_load_segments(buf, size, eh) < 0) {
        kfree(buf); return -6;
    }
    void* ustk = kmalloc(ELF_USER_STACK_SIZE);
    if (!ustk) { kfree(buf); return -7; }
    uint32_t entry = eh->e_entry;
    const char* name = path_basename(path);
    int tid = task_create_named(elf_task_wrapper, 0, TASK_DEFAULT_QUANTUM, name);
    kfree(buf);
    if (tid < 0) {
        kfree(ustk);
        return tid;
    }

    if (elf_launch_slot_set(tid, entry, (uintptr_t)ustk) < 0) {
        task_kill(tid);
        return -10;
    }

    if (stdin_fd >= 0 && task_clone_fd_to_task(tid, 0, stdin_fd) < 0) {
        elf_launch_slot_clear(tid);
        task_kill(tid);
        return -8;
    }
    if (stdout_fd >= 0 && task_clone_fd_to_task(tid, 1, stdout_fd) < 0) {
        elf_launch_slot_clear(tid);
        task_kill(tid);
        return -9;
    }

    if (wait) {
        int status = 0;
        (void)task_waitpid(tid, &status, 0);
    }
    return tid;
}

int elf_exec(const char* path, int wait) {
    return elf_exec_with_stdio(path, wait, -1, -1);
}

int elf_execve_current(const char* path, struct registers* r) {
    if (!path || !r) return -1;

    uint8_t* buf = NULL;
    uint32_t size = 0;
    int io = elf_read_from_vfs(path, &buf, &size);
    if (io < 0) return io;

    int err = elf_validate(buf, size);
    if (err < 0) {
        kfree(buf);
        return err - 10;
    }

    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)buf;
    if (elf_load_segments(buf, size, eh) < 0) {
        kfree(buf);
        return -6;
    }

    void* ustk = kmalloc(ELF_USER_STACK_SIZE);
    if (!ustk) {
        kfree(buf);
        return -7;
    }

    uint32_t user_esp = (uint32_t)((uintptr_t)ustk + ELF_USER_STACK_SIZE) & ~0xFu;
    r->eip = eh->e_entry;
    r->useresp = user_esp;
    r->eax = 0;

    /* POSIX-like behavior: apply CLOEXEC only on successful image replacement. */
    task_close_cloexec_fds_current();

    const char* name = path_basename(path);
    if (name) task_set_current_name(name);

    kfree(buf);
    return 0;
}
/*
 * Hand-assembled i386 ELF: prints "Hello from ELF!\n" and exits.
 * Loaded at vaddr 0x08048000, entry 0x08048054, string at 0x08048079.
 */
static const uint8_t test_elf_hello[] = {
    /* ELF header (52 bytes) */
    0x7f,'E','L','F', 0x01,0x01,0x01,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x00, 0x03,0x00, 0x01,0x00,0x00,0x00,
    0x54,0x80,0x04,0x08, 0x34,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x34,0x00, 0x20,0x00, 0x01,0x00,
    0x00,0x00, 0x00,0x00, 0x00,0x00,
    /* Program header (32 bytes) */
    0x01,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x80,0x04,0x08, 0x00,0x80,0x04,0x08,
    0x89,0x00,0x00,0x00, 0x89,0x00,0x00,0x00,
    0x05,0x00,0x00,0x00, 0x00,0x10,0x00,0x00,
    /* Code at offset 0x54 */
    0xb8,0x01,0x00,0x00,0x00,  /* mov eax,1  (SYS_write) */
    0xbb,0x01,0x00,0x00,0x00,  /* mov ebx,1  (stdout)    */
    0xb9,0x79,0x80,0x04,0x08,  /* mov ecx,msg            */
    0xba,0x10,0x00,0x00,0x00,  /* mov edx,16             */
    0xcd,0x80,                  /* int 0x80               */
    0xb8,0x03,0x00,0x00,0x00,  /* mov eax,3  (SYS_exit)  */
    0xbb,0x00,0x00,0x00,0x00,  /* mov ebx,0              */
    0xcd,0x80,                  /* int 0x80               */
    0xf4, 0xeb,0xfd,           /* hlt; jmp $-1           */
    /* String at offset 0x79: "Hello from ELF!\n" */
    'H','e','l','l','o',' ','f','r','o','m',' ','E','L','F','!','\n'
};

void elf_install_test_programs(void) {
    vfs_node_t* root = vfs_get_root();
    if (!root) return;
    vfs_create(root, "bin", VFS_DIRECTORY);
    vfs_node_t* bin = vfs_finddir(root, "bin");
    if (!bin) return;
    vfs_create(bin, "hello", VFS_FILE);
    vfs_node_t* hello = vfs_finddir(bin, "hello");
    if (!hello) return;
    vfs_write(hello, 0, sizeof(test_elf_hello), test_elf_hello);

    vfs_create(bin, "anim", VFS_FILE);
    vfs_node_t* anim = vfs_finddir(bin, "anim");
    if (!anim) return;
    uint32_t anim_size = (uint32_t)(_binary_build_user_anim_elf_end - _binary_build_user_anim_elf_start);
    vfs_write(anim, 0, anim_size, _binary_build_user_anim_elf_start);

    vfs_create(bin, "forktest", VFS_FILE);
    vfs_node_t* forktest = vfs_finddir(bin, "forktest");
    if (!forktest) return;
    uint32_t forktest_size = (uint32_t)(_binary_build_user_forktest_elf_end - _binary_build_user_forktest_elf_start);
    vfs_write(forktest, 0, forktest_size, _binary_build_user_forktest_elf_start);

    vfs_create(bin, "execvetest", VFS_FILE);
    vfs_node_t* execvetest = vfs_finddir(bin, "execvetest");
    if (!execvetest) return;
    uint32_t execvetest_size = (uint32_t)(_binary_build_user_execvetest_elf_end - _binary_build_user_execvetest_elf_start);
    vfs_write(execvetest, 0, execvetest_size, _binary_build_user_execvetest_elf_start);

    vfs_create(bin, "waittest", VFS_FILE);
    vfs_node_t* waittest = vfs_finddir(bin, "waittest");
    if (!waittest) return;
    uint32_t waittest_size = (uint32_t)(_binary_build_user_waittest_elf_end - _binary_build_user_waittest_elf_start);
    vfs_write(waittest, 0, waittest_size, _binary_build_user_waittest_elf_start);

    vfs_create(bin, "waitstress", VFS_FILE);
    vfs_node_t* waitstress = vfs_finddir(bin, "waitstress");
    if (!waitstress) return;
    uint32_t waitstress_size = (uint32_t)(_binary_build_user_waitstress_elf_end - _binary_build_user_waitstress_elf_start);
    vfs_write(waitstress, 0, waitstress_size, _binary_build_user_waitstress_elf_start);

    vfs_create(bin, "waitstressbg", VFS_FILE);
    vfs_node_t* waitstressbg = vfs_finddir(bin, "waitstressbg");
    if (!waitstressbg) return;
    uint32_t waitstressbg_size = (uint32_t)(_binary_build_user_waitstressbg_elf_end - _binary_build_user_waitstressbg_elf_start);
    vfs_write(waitstressbg, 0, waitstressbg_size, _binary_build_user_waitstressbg_elf_start);

    vfs_create(bin, "catfd", VFS_FILE);
    vfs_node_t* catfd = vfs_finddir(bin, "catfd");
    if (!catfd) return;
    uint32_t catfd_size = (uint32_t)(_binary_build_user_catfd_elf_end - _binary_build_user_catfd_elf_start);
    vfs_write(catfd, 0, catfd_size, _binary_build_user_catfd_elf_start);
}
