/* i686 System V ABI
   void context_switch(struct context* old, struct context* new);
   Expects:
     [esp+4] = old
     [esp+8] = new
   Saves: ebx, esi, edi, ebp, esp, eip into *old
   Restores from *new and continues at new->eip.
*/

    .section .text
    .global context_switch
    .type context_switch, @function

context_switch:
    /* stack: retaddr, old, new */
    movl 4(%esp), %eax      /* eax = old */
    movl 8(%esp), %edx      /* edx = new */

    /* save callee-saved regs into *old */
    movl %ebx, 0(%eax)
    movl %esi, 4(%eax)
    movl %edi, 8(%eax)
    movl %ebp, 12(%eax)

    /* save esp as it will be after we return (skip retaddr, old, new) */
    leal 12(%esp), %ecx
    movl %ecx, 16(%eax)

    /* save eip (the return address) */
    movl (%esp), %ecx
    movl %ecx, 20(%eax)

    /* restore callee-saved regs from *new */
    movl 0(%edx), %ebx
    movl 4(%edx), %esi
    movl 8(%edx), %edi
    movl 12(%edx), %ebp
    movl 16(%edx), %esp

    /* jump to new->eip */
    movl 20(%edx), %ecx
    jmp *%ecx

    .size context_switch, .-context_switch