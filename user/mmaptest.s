.section .text
.global _start

/* syscall numbers */
.set SYS_write,   1
.set SYS_exit,    3
.set SYS_open,    4
.set SYS_close,   6
.set SYS_mmap,   27
.set SYS_munmap, 28

/* mmap flags/prot */
.set PROT_READ,     0x1
.set PROT_WRITE,    0x2
.set MAP_PRIVATE,   0x02
.set MAP_ANONYMOUS, 0x20

_start:
    /* Anonymous mapping: read/write a byte, then unmap. */
    movl $0, anon_args_addr
    movl $4096, anon_args_len
    movl $(PROT_READ | PROT_WRITE), anon_args_prot
    movl $(MAP_PRIVATE | MAP_ANONYMOUS), anon_args_flags
    movl $-1, anon_args_fd
    movl $0, anon_args_off

    movl $SYS_mmap, %eax
    movl $anon_args, %ebx
    int $0x80
    cmpl $-1, %eax
    je fail
    movl %eax, anon_ptr

    movl anon_ptr, %edi
    movb $0x5a, (%edi)
    cmpb $0x5a, (%edi)
    jne fail

    movl $SYS_munmap, %eax
    movl anon_ptr, %ebx
    movl $4096, %ecx
    int $0x80
    cmpl $0, %eax
    jl fail

    /* File-backed mapping: map /bin/hello and verify ELF magic. */
    movl $SYS_open, %eax
    movl $path_hello, %ebx
    xorl %ecx, %ecx
    int $0x80
    cmpl $0, %eax
    jl fail
    movl %eax, file_fd

    movl $0, file_args_addr
    movl $4096, file_args_len
    movl $PROT_READ, file_args_prot
    movl $MAP_PRIVATE, file_args_flags
    movl file_fd, %eax
    movl %eax, file_args_fd
    movl $0, file_args_off

    movl $SYS_mmap, %eax
    movl $file_args, %ebx
    int $0x80
    cmpl $-1, %eax
    je fail
    movl %eax, file_ptr

    movl file_ptr, %esi
    cmpb $0x7f, 0(%esi)
    jne fail
    cmpb $'E', 1(%esi)
    jne fail
    cmpb $'L', 2(%esi)
    jne fail
    cmpb $'F', 3(%esi)
    jne fail

    movl $SYS_munmap, %eax
    movl file_ptr, %ebx
    movl $4096, %ecx
    int $0x80
    cmpl $0, %eax
    jl fail

    movl $SYS_close, %eax
    movl file_fd, %ebx
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

fail:
    movl file_fd, %ebx
    cmpl $0, %ebx
    jl skip_close
    movl $SYS_close, %eax
    int $0x80

skip_close:
    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_fail, %ecx
    movl $msg_fail_len, %edx
    int $0x80

    movl $SYS_exit, %eax
    movl $1, %ebx
    int $0x80

.section .data
anon_ptr:
    .long 0
file_ptr:
    .long 0
file_fd:
    .long -1

/* syscall_mmap_args_t layout */
anon_args:
anon_args_addr:
    .long 0
anon_args_len:
    .long 0
anon_args_prot:
    .long 0
anon_args_flags:
    .long 0
anon_args_fd:
    .long -1
anon_args_off:
    .long 0

file_args:
file_args_addr:
    .long 0
file_args_len:
    .long 0
file_args_prot:
    .long 0
file_args_flags:
    .long 0
file_args_fd:
    .long -1
file_args_off:
    .long 0

.section .rodata
path_hello:
    .asciz "/bin/hello"

msg_ok:
    .ascii "[mmaptest] PASS\n"
.set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "[mmaptest] FAIL\n"
.set msg_fail_len, . - msg_fail

