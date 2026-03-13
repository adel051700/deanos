/*
 * elf.c — Minimal ELF32 loader for DeanOS
 */
#include "include/kernel/elf.h"
#include "include/kernel/vfs.h"
#include "include/kernel/kheap.h"
#include "include/kernel/paging.h"
#include "include/kernel/task.h"
#include "include/kernel/usermode.h"
#include <string.h>
#include <stdio.h>
#define ELF_USER_STACK_SIZE  (8u * 1024u)
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
static struct { uint32_t entry; uintptr_t ustk; } g_elf_launch;
static void elf_task_wrapper(void) {
    uint32_t  entry    = g_elf_launch.entry;
    uintptr_t ustk     = g_elf_launch.ustk;
    uint32_t  user_esp = (uint32_t)(ustk + ELF_USER_STACK_SIZE) & ~0xFu;
    enter_usermode(entry, user_esp);
}
int elf_exec(const char* path, int wait) {
    if (!path) return -1;
    vfs_node_t* node = vfs_namei(path);
    if (!node)                     return -1;
    if (!(node->type & VFS_FILE)) return -2;
    if (node->size == 0)           return -3;
    uint8_t* buf = (uint8_t*)kmalloc(node->size);
    if (!buf) return -4;
    int32_t nread = vfs_read(node, 0, node->size, buf);
    if (nread <= 0) { kfree(buf); return -5; }
    int err = elf_validate(buf, (uint32_t)nread);
    if (err < 0) { kfree(buf); return err - 10; }
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)buf;
    if (elf_load_segments(buf, (uint32_t)nread, eh) < 0) {
        kfree(buf); return -6;
    }
    void* ustk = kmalloc(ELF_USER_STACK_SIZE);
    if (!ustk) { kfree(buf); return -7; }
    g_elf_launch.entry = eh->e_entry;
    g_elf_launch.ustk  = (uintptr_t)ustk;
    const char* name = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' && *(p + 1)) name = p + 1;
    int tid = task_create_named(elf_task_wrapper, 0, TASK_DEFAULT_QUANTUM, name);
    kfree(buf);
    if (tid < 0) return tid;
    if (wait) task_wait(tid);
    return tid;
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

/*
 * Small spinner animation ELF:
 * prints "\r|", "\r/", "\r-", "\r\\" in a loop, then "\rDone\n" and exits.
 */
static const uint8_t test_elf_anim[] = {
    0x7f, 0x45, 0x4c, 0x46, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x54, 0x80, 0x04, 0x08, 0x34, 0x00, 0x00, 0x00,
    0x10, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x34, 0x00, 0x20, 0x00, 0x01, 0x00, 0x28, 0x00,
    0x06, 0x00, 0x05, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x04, 0x08, 0x00, 0x80, 0x04, 0x08,
    0xbc, 0x00, 0x00, 0x00, 0xbc, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0xbe, 0xae, 0x80, 0x04, 0x08, 0xbd, 0x20, 0x00,
    0x00, 0x00, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xbb,
    0x01, 0x00, 0x00, 0x00, 0x89, 0xf1, 0xba, 0x02,
    0x00, 0x00, 0x00, 0xcd, 0x80, 0xbf, 0x00, 0x00,
    0x80, 0x01, 0x4f, 0x75, 0xfd, 0x83, 0xc6, 0x02,
    0x81, 0xfe, 0xb6, 0x80, 0x04, 0x08, 0x72, 0x05,
    0xbe, 0xae, 0x80, 0x04, 0x08, 0x4d, 0x75, 0xd2,
    0xb8, 0x01, 0x00, 0x00, 0x00, 0xbb, 0x01, 0x00,
    0x00, 0x00, 0xb9, 0xb6, 0x80, 0x04, 0x08, 0xba,
    0x06, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8, 0x03,
    0x00, 0x00, 0x00, 0x31, 0xdb, 0xcd, 0x80, 0xf4,
    0xeb, 0xfd, 0x0d, 0x7c, 0x0d, 0x2f, 0x0d, 0x2d,
    0x0d, 0x5c, 0x0d, 0x44, 0x6f, 0x6e, 0x65, 0x0a,
    0x00
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
    vfs_write(anim, 0, sizeof(test_elf_anim), test_elf_anim);
}
