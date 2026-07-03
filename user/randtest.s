.section .text
.global _start

/* --- helper: write 16 raw bytes at %esi to stdout as 32 hex chars + newline ---
 * clobbers eax,ebx,ecx,edx,edi,esi. Expects hexbuf (33 bytes) in .bss. */
emit_hex16:
    movl $hexbuf, %edi
    movl $16, %ecx
1:
    movzbl (%esi), %eax        /* current byte */
    movl %eax, %edx
    shrl $4, %edx              /* high nibble */
    andl $0x0f, %edx
    movb hexits(%edx), %bl
    movb %bl, (%edi)
    incl %edi
    movl %eax, %edx
    andl $0x0f, %edx           /* low nibble */
    movb hexits(%edx), %bl
    movb %bl, (%edi)
    incl %edi
    incl %esi
    loop 1b
    movb $10, (%edi)           /* newline */
    /* write 33 bytes */
    movl $1, %eax              /* SYS_write */
    movl $1, %ebx              /* stdout */
    movl $hexbuf, %ecx
    movl $33, %edx
    int $0x80
    ret

_start:
    /* 1) getrandom(buf, 16, 0) */
    movl $50, %eax             /* SYS_getrandom */
    movl $buf, %ebx
    movl $16, %ecx
    movl $0, %edx
    int $0x80
    movl $buf, %esi
    call emit_hex16

    /* 2) fd = open("/dev/urandom", 0); read 16; print */
    movl $4, %eax              /* SYS_open */
    movl $path_urandom, %ebx
    movl $0, %ecx              /* O_RDONLY */
    int $0x80
    movl %eax, %edi            /* save fd */
    movl $5, %eax              /* SYS_read */
    movl %edi, %ebx
    movl $buf, %ecx
    movl $16, %edx
    int $0x80
    movl $buf, %esi
    call emit_hex16

    /* 3) fd = open("/dev/random", 0); read 16; print */
    movl $4, %eax
    movl $path_random, %ebx
    movl $0, %ecx
    int $0x80
    movl %eax, %edi
    movl $5, %eax
    movl %edi, %ebx
    movl $buf, %ecx
    movl $16, %edx
    int $0x80
    movl $buf, %esi
    call emit_hex16

    /* exit(0) */
    movl $3, %eax
    xorl %ebx, %ebx
    int $0x80

.section .rodata
hexits:
    .ascii "0123456789abcdef"
path_urandom:
    .asciz "/dev/urandom"
path_random:
    .asciz "/dev/random"

.section .bss
.align 16
buf:
    .skip 16
hexbuf:
    .skip 33
