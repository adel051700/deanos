.section .text
.global _start

.set SYS_write,      1
.set SYS_exit,       3
.set SYS_fork,      14
.set SYS_waitpid,   16
.set SYS_mmap,      27
.set SYS_munmap,    28
.set SYS_shm_open,  29
.set SYS_shm_unlink,30

.set PROT_READ,     0x1
.set PROT_WRITE,    0x2
.set MAP_SHARED,    0x01
.set MAP_SHM,       0x40
.set SHM_CREATE,    0x1

.set SHM_KEY,       1234

_start:
    /* Create shared object. */
    movl $SYS_shm_open, %eax
    movl $SHM_KEY, %ebx
    movl $4096, %ecx
    movl $SHM_CREATE, %edx
    int $0x80
    cmpl $0, %eax
    jle fail
    movl %eax, shm_id

    /* Map shared region in parent. */
    movl $0, map_args_addr
    movl $4096, map_args_len
    movl $(PROT_READ | PROT_WRITE), map_args_prot
    movl $(MAP_SHARED | MAP_SHM), map_args_flags
    movl shm_id, %eax
    movl %eax, map_args_fd
    movl $0, map_args_off

    movl $SYS_mmap, %eax
    movl $map_args, %ebx
    int $0x80
    cmpl $-1, %eax
    je fail
    movl %eax, map_ptr

    movl map_ptr, %edi
    movb $0, (%edi)

    /* Child writes through shared mapping. */
    movl $SYS_fork, %eax
    xorl %ebx, %ebx
    int $0x80
    cmpl $0, %eax
    jl fail
    je child_path

parent_path:
    movl $SYS_waitpid, %eax
    movl $-1, %ebx
    movl $status_word, %ecx
    xorl %edx, %edx
    int $0x80
    cmpl $0, %eax
    jle fail

    movl map_ptr, %edi
    cmpb $0x5a, (%edi)
    jne fail

    movl $SYS_munmap, %eax
    movl map_ptr, %ebx
    movl $4096, %ecx
    int $0x80
    cmpl $0, %eax
    jl fail

    movl $SYS_shm_unlink, %eax
    movl $SHM_KEY, %ebx
    int $0x80
    cmpl $0, %eax
    jl fail

    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_ok, %ecx
    movl $msg_ok_len, %edx
    int $0x80

    movl $SYS_exit, %eax
    xorl %ebx, %ebx
    int $0x80

child_path:
    movl map_ptr, %edi
    movb $0x5a, (%edi)

    movl $SYS_exit, %eax
    xorl %ebx, %ebx
    int $0x80

fail:
    movl $SYS_munmap, %eax
    movl map_ptr, %ebx
    movl $4096, %ecx
    cmpl $0, %ebx
    jle skip_munmap
    int $0x80

skip_munmap:
    movl $SYS_shm_unlink, %eax
    movl $SHM_KEY, %ebx
    int $0x80

    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_fail, %ecx
    movl $msg_fail_len, %edx
    int $0x80

    movl $SYS_exit, %eax
    movl $1, %ebx
    int $0x80

.section .data
shm_id:
    .long 0
map_ptr:
    .long 0
status_word:
    .long 0

/* syscall_mmap_args_t layout */
map_args:
map_args_addr:
    .long 0
map_args_len:
    .long 0
map_args_prot:
    .long 0
map_args_flags:
    .long 0
map_args_fd:
    .long -1
map_args_off:
    .long 0

.section .rodata
msg_ok:
    .ascii "[shmtest] PASS\n"
.set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "[shmtest] FAIL\n"
.set msg_fail_len, . - msg_fail

