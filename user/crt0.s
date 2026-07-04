.section .text
.global _start
.extern main
.extern exit

_start:
    movl (%esp), %eax      /* argc, per the ABI elf_build_argv_stack writes */
    leal 4(%esp), %ecx     /* argv */
    pushl %ecx
    pushl %eax
    call main
    addl $8, %esp
    pushl %eax             /* main's return value -> exit status */
    call exit

1:
    hlt
    jmp 1b
