/*
 * CPU fault handling for faults originating in user mode.
 *
 * The non-executable user stack (see gdt.c) relies on a #GP being raised when
 * the CPU tries to fetch an instruction from above the ring-3 code-segment
 * limit. Without a handler, isr_handler() would print the exception and halt
 * the whole kernel — turning a hardening feature into a denial of service.
 *
 * This handler instead terminates only the offending task when the fault comes
 * from ring 3. A ring-0 #GP is a genuine kernel bug, so we leave it to the
 * generic panic path.
 */
#include "include/kernel/fault.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/task.h"
#include "include/kernel/log.h"
#include "include/kernel/tty.h"
#include <stdio.h>

#define INT_GENERAL_PROTECTION 13

/* Conventional "killed by SIGSEGV" wait status (128 + signal number). */
#define USER_FAULT_STATUS (128u + 11u)

static void panic_from_kernel(const char* what, struct registers* regs) {
    char buf[16];
    terminal_writestring("\nKERNEL FAULT: ");
    terminal_writestring(what);
    itoa(regs->err_code, buf, 16);
    terminal_writestring(" err=0x"); terminal_writestring(buf);
    itoa(regs->eip, buf, 16);
    terminal_writestring(" EIP=0x"); terminal_writestring(buf);
    itoa(regs->cs, buf, 16);
    terminal_writestring(" CS=0x"); terminal_writestring(buf);
    terminal_writestring("\nSystem halted.\n");
    for (;;) __asm__ __volatile__("hlt");
}

static void gp_fault_handler(struct registers* regs) {
    if ((regs->cs & 0x3u) == 0x3u) {
        /* Ring-3 fault: most likely an instruction fetch from the (now
         * non-executable) user stack, or another segment-limit/privilege
         * violation. Kill just this process, like a default SIGSEGV. */
        klog("user #GP: task terminated (non-executable stack or bad segment access)");
        task_exit_with_status(USER_FAULT_STATUS); /* yields away, never returns */
        return; /* unreachable */
    }
    panic_from_kernel("General Protection Fault", regs);
}

void fault_initialize(void) {
    register_interrupt_handler(INT_GENERAL_PROTECTION, gp_fault_handler);
}
