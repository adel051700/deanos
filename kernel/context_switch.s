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
