.section .text
.global _start

.equ SYS_write, 1
.equ SYS_exit,  3
.equ SYS_read,  5
.equ SYS_sleep_ms, 10
.equ FRAME_COUNT, 28
.equ FRAME_DELAY_MS, 90

_start:
    sub $16, %esp
    movl $0, 8(%esp)

    mov $SYS_write, %eax
    mov $1, %ebx
    mov $intro, %ecx
    mov $intro_len, %edx
    int $0x80

frame_loop:
    mov 8(%esp), %eax
    mov frame_ptrs(,%eax,4), %ecx
    mov frame_lens(,%eax,4), %edx
    mov $SYS_write, %eax
    mov $1, %ebx
    int $0x80

    mov $SYS_sleep_ms, %eax
    mov $FRAME_DELAY_MS, %ebx
    int $0x80

    mov $SYS_read, %eax
    mov $0, %ebx
    lea 12(%esp), %ecx
    mov $1, %edx
    int $0x80
    cmp $1, %eax
    jne next_frame
    cmpb $10, 12(%esp)
    je anim_done
    cmpb $13, 12(%esp)
    je anim_done

next_frame:
    mov 8(%esp), %eax
    incl %eax
    cmp $FRAME_COUNT, %eax
    jb store_frame
    xor %eax, %eax
store_frame:
    mov %eax, 8(%esp)
    jmp frame_loop

anim_done:
    mov $SYS_write, %eax
    mov $1, %ebx
    mov $done_line, %ecx
    mov $done_line_len, %edx
    int $0x80

    mov $SYS_exit, %eax
    xor %ebx, %ebx
    int $0x80

hang:
    hlt
    jmp hang

.section .rodata
intro:
    .ascii "anim (ELF): press Enter to stop\n"
intro_end:
    .equ intro_len, intro_end - intro

frame0:
    .ascii "\r[<=>............................] DeanOS ELF demo"
frame0_end:
    .equ frame0_len, frame0_end - frame0

frame1:
    .ascii "\r[..<=>..........................] DeanOS ELF demo"
frame1_end:
    .equ frame1_len, frame1_end - frame1

frame2:
    .ascii "\r[....<=>........................] DeanOS ELF demo"
frame2_end:
    .equ frame2_len, frame2_end - frame2

frame3:
    .ascii "\r[......<=>......................] DeanOS ELF demo"
frame3_end:
    .equ frame3_len, frame3_end - frame3

frame4:
    .ascii "\r[........<=>....................] DeanOS ELF demo"
frame4_end:
    .equ frame4_len, frame4_end - frame4

frame5:
    .ascii "\r[..........<=>..................] DeanOS ELF demo"
frame5_end:
    .equ frame5_len, frame5_end - frame5

frame6:
    .ascii "\r[............<=>................] DeanOS ELF demo"
frame6_end:
    .equ frame6_len, frame6_end - frame6

frame7:
    .ascii "\r[..............<=>..............] DeanOS ELF demo"
frame7_end:
    .equ frame7_len, frame7_end - frame7

frame8:
    .ascii "\r[................<=>............] DeanOS ELF demo"
frame8_end:
    .equ frame8_len, frame8_end - frame8

frame9:
    .ascii "\r[..................<=>..........] DeanOS ELF demo"
frame9_end:
    .equ frame9_len, frame9_end - frame9

frame10:
    .ascii "\r[....................<=>........] DeanOS ELF demo"
frame10_end:
    .equ frame10_len, frame10_end - frame10

frame11:
    .ascii "\r[......................<=>......] DeanOS ELF demo"
frame11_end:
    .equ frame11_len, frame11_end - frame11

frame12:
    .ascii "\r[........................<=>....] DeanOS ELF demo"
frame12_end:
    .equ frame12_len, frame12_end - frame12

frame13:
    .ascii "\r[..........................<=>..] DeanOS ELF demo"
frame13_end:
    .equ frame13_len, frame13_end - frame13

frame14:
    .ascii "\r[............................<=>] DeanOS ELF demo"
frame14_end:
    .equ frame14_len, frame14_end - frame14

frame15:
    .ascii "\r[..........................<=>..] DeanOS ELF demo"
frame15_end:
    .equ frame15_len, frame15_end - frame15

frame16:
    .ascii "\r[........................<=>....] DeanOS ELF demo"
frame16_end:
    .equ frame16_len, frame16_end - frame16

frame17:
    .ascii "\r[......................<=>......] DeanOS ELF demo"
frame17_end:
    .equ frame17_len, frame17_end - frame17

frame18:
    .ascii "\r[....................<=>........] DeanOS ELF demo"
frame18_end:
    .equ frame18_len, frame18_end - frame18

frame19:
    .ascii "\r[..................<=>..........] DeanOS ELF demo"
frame19_end:
    .equ frame19_len, frame19_end - frame19

frame20:
    .ascii "\r[................<=>............] DeanOS ELF demo"
frame20_end:
    .equ frame20_len, frame20_end - frame20

frame21:
    .ascii "\r[..............<=>..............] DeanOS ELF demo"
frame21_end:
    .equ frame21_len, frame21_end - frame21

frame22:
    .ascii "\r[............<=>................] DeanOS ELF demo"
frame22_end:
    .equ frame22_len, frame22_end - frame22

frame23:
    .ascii "\r[..........<=>..................] DeanOS ELF demo"
frame23_end:
    .equ frame23_len, frame23_end - frame23

frame24:
    .ascii "\r[........<=>....................] DeanOS ELF demo"
frame24_end:
    .equ frame24_len, frame24_end - frame24

frame25:
    .ascii "\r[......<=>......................] DeanOS ELF demo"
frame25_end:
    .equ frame25_len, frame25_end - frame25

frame26:
    .ascii "\r[....<=>........................] DeanOS ELF demo"
frame26_end:
    .equ frame26_len, frame26_end - frame26

frame27:
    .ascii "\r[..<=>..........................] DeanOS ELF demo"
frame27_end:
    .equ frame27_len, frame27_end - frame27

done_line:
    .ascii "\r[stopped........................] DeanOS ELF demo done\n"
done_line_end:
    .equ done_line_len, done_line_end - done_line

.align 4
frame_ptrs:
    .long frame0, frame1, frame2, frame3, frame4, frame5, frame6, frame7
    .long frame8, frame9, frame10, frame11, frame12, frame13, frame14, frame15
    .long frame16, frame17, frame18, frame19, frame20, frame21, frame22, frame23
    .long frame24, frame25, frame26, frame27

frame_lens:
    .long frame0_len, frame1_len, frame2_len, frame3_len, frame4_len, frame5_len, frame6_len, frame7_len
    .long frame8_len, frame9_len, frame10_len, frame11_len, frame12_len, frame13_len, frame14_len, frame15_len
    .long frame16_len, frame17_len, frame18_len, frame19_len, frame20_len, frame21_len, frame22_len, frame23_len
    .long frame24_len, frame25_len, frame26_len, frame27_len


