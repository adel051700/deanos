/* i686 System V ABI
   void context_switch(struct context* old, struct context* new);
   Expects:
     [esp+4] = old
     [esp+8] = new
   Saves: ebx, esi, edi, ebp, esp, eip into *old
   Restores from *new and continues at new->eip.
*/

    .section .text

/* ---- context_switch ---------------------------------------------------- */
    .global context_switch
    .type context_switch, @function

context_switch:
    movl 4(%esp), %eax          /* eax = old ctx */
    movl 8(%esp), %edx          /* edx = new ctx */

    /* Save callee-saved regs on the CURRENT stack */
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    /* Save current ESP into old->esp  (offset 0) */
    movl %esp, (%eax)

    /* Load new ESP from new->esp  (offset 0) */
    movl (%edx), %esp

    /* Restore callee-saved regs from the NEW stack */
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp

    ret                           /* pops return address and jumps there */

    .size context_switch, .-context_switch

/* ---- task_trampoline --------------------------------------------------- */
/*
 * First-time entry point for every new task.
 * context_switch's "ret" lands here.
 * ebx = real task entry function (set up by task_create).
 */
    .global task_trampoline
    .type task_trampoline, @function

task_trampoline:
    sti                           /* re-enable interrupts */
    call *%ebx                    /* call the real task entry */

    /* Task function returned — mark it dead and yield. */
    call task_exit

    /* Should never reach here, but just in case: */
    cli
.Lhalt:
    hlt
    jmp .Lhalt

    .size task_trampoline, .-task_trampoline

/* ---- enter_usermode ---------------------------------------------------- */
/*
 * void enter_usermode(uint32_t entry, uint32_t user_esp);
 *
 * Drops to ring 3 via iret.
 * Sets up the stack frame that iret expects for a privilege-level change:
 *   [SS] [ESP] [EFLAGS] [CS] [EIP]
 *
 * GDT selectors (with RPL 3):
 *   User code  = 0x18 | 3 = 0x1B
 *   User data  = 0x20 | 3 = 0x23
 */
    .global enter_usermode
    .type enter_usermode, @function

enter_usermode:
    movl 4(%esp), %ecx          /* ecx = entry point (EIP)    */
    movl 8(%esp), %edx          /* edx = user stack (ESP)     */

    cli                          /* disable interrupts during setup */

    /* Load user data segment into DS, ES, FS, GS */
    movl $0x23, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    /* Build iret frame on kernel stack */
    pushl $0x23                  /* SS  = user data (RPL 3)    */
    pushl %edx                   /* ESP = user stack pointer   */
    pushfl                       /* EFLAGS                     */
    orl $0x200, (%esp)           /* ensure IF=1 in user mode   */
    pushl $0x1B                  /* CS  = user code (RPL 3)    */
    pushl %ecx                   /* EIP = entry point          */
    iret

    .size enter_usermode, .-enter_usermode

