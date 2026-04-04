global _start
section .rodata
    info_msg: db "dns: userland DNS utility", 0x0a
    info_len: equ $ - info_msg
    usage_msg: db "usage: dns <hostname> [dns_server]", 0x0a
    usage_len: equ $ - usage_msg
    help_msg: db "  Example: dns google.com 8.8.8.8", 0x0a
    help_len: equ $ - help_msg
    note_msg: db "  Note: Use 'net dns' shell command for DNS resolution", 0x0a
    note_len: equ $ - note_msg
section .text
_start:
    mov eax, [esp]
    cmp eax, 2
    jl show_usage
    mov eax, 1
    mov bl, 1
    lea ecx, [rel info_msg]
    mov edx, info_len
    int $0x80
    mov eax, 1
    mov bl, 1
    lea ecx, [rel help_msg]
    mov edx, help_len
    int $0x80
    mov eax, 1
    mov bl, 1
    lea ecx, [rel note_msg]
    mov edx, note_len
    int $0x80
    jmp exit_ok
show_usage:
    mov eax, 1
    mov bl, 1
    lea ecx, [rel usage_msg]
    mov edx, usage_len
    int $0x80
    mov eax, 1
    mov bl, 1
    lea ecx, [rel help_msg]
    mov edx, help_len
    int $0x80
exit_ok:
    mov eax, 1
    xor ebx, ebx
    int $0x80
