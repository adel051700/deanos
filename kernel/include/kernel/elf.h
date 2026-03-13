#ifndef _KERNEL_ELF_H
#define _KERNEL_ELF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ELF32 types ------------------------------------------------------- */

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef  int32_t Elf32_Sword;
typedef uint32_t Elf32_Word;

/* ---- e_ident indices --------------------------------------------------- */
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6
#define EI_OSABI    7
#define EI_NIDENT   16

/* e_ident[EI_CLASS] */
#define ELFCLASS32  1

/* e_ident[EI_DATA] */
#define ELFDATA2LSB 1

/* ---- ELF header -------------------------------------------------------- */

typedef struct {
    uint8_t     e_ident[EI_NIDENT];
    Elf32_Half  e_type;
    Elf32_Half  e_machine;
    Elf32_Word  e_version;
    Elf32_Addr  e_entry;
    Elf32_Off   e_phoff;
    Elf32_Off   e_shoff;
    Elf32_Word  e_flags;
    Elf32_Half  e_ehsize;
    Elf32_Half  e_phentsize;
    Elf32_Half  e_phnum;
    Elf32_Half  e_shentsize;
    Elf32_Half  e_shnum;
    Elf32_Half  e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

/* e_type */
#define ET_EXEC     2

/* e_machine */
#define EM_386      3

/* ---- Program header ---------------------------------------------------- */

typedef struct {
    Elf32_Word  p_type;
    Elf32_Off   p_offset;
    Elf32_Addr  p_vaddr;
    Elf32_Addr  p_paddr;
    Elf32_Word  p_filesz;
    Elf32_Word  p_memsz;
    Elf32_Word  p_flags;
    Elf32_Word  p_align;
} __attribute__((packed)) Elf32_Phdr;

/* p_type */
#define PT_NULL     0
#define PT_LOAD     1

/* p_flags */
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

/* ---- Public API -------------------------------------------------------- */

/*
 * Validate raw ELF data: checks magic, class, endianness, type, machine.
 * Returns 0 on success, negative on error.
 */
int elf_validate(const uint8_t* data, uint32_t size);

/*
 * Load an ELF executable from the VFS, map its segments into user address
 * space, create a user-mode task, and return the new task ID (>0).
 * Returns negative on error.
 *
 * If `wait` is non-zero the caller blocks until the task exits.
 */
int elf_exec(const char* path, int wait);

/*
 * Write built-in demo ELF programs into the ramfs so the user can run
 * them with "exec /bin/hello".  Call after ramfs_initialize().
 */
void elf_install_test_programs(void);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_ELF_H */

