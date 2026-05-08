/*
 * stack_protector.c — backing for GCC's -fstack-protector instrumentation.
 *
 * This file must be compiled with -fno-stack-protector (see Makefile rule):
 * the failure path cannot itself rely on canaries, and reseeding the global
 * guard while a caller's frame still references the old value is fine only
 * because this file's own frames do not carry canaries.
 */
#include "include/kernel/stack_protector.h"
#include "include/kernel/log.h"
#include "include/kernel/serial.h"
#include "include/kernel/tty.h"
#include "include/kernel/pit.h"
#include "include/kernel/rtc.h"
#include <stdio.h>

/* Non-zero compile-time seed so canary checks pass during early boot, before
 * stack_protector_initialize() can mix in runtime entropy. The low byte is
 * non-zero so string-terminator overruns can't silently match it. */
uintptr_t __stack_chk_guard = 0xe2dee396u;

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void stack_protector_initialize(void) {
    uint64_t tsc = read_tsc();
    rtc_time_t t = {0};
    rtc_read_time(&t);

    uint32_t mix = (uint32_t)(tsc ^ (tsc >> 32));
    mix ^= (uint32_t)pit_get_ticks();
    mix ^= ((uint32_t)t.second << 24) | ((uint32_t)t.minute << 16) |
           ((uint32_t)t.hour   <<  8) | (uint32_t)t.day;
    mix ^= 0xdeadbeefu;
    /* Force a non-zero low byte so NUL-terminator-style overruns trip. */
    if ((mix & 0xffu) == 0u) mix |= 0xa5u;

    __stack_chk_guard = (uintptr_t)mix;
}

__attribute__((noreturn))
void __stack_chk_fail(void) {
    __asm__ __volatile__("cli");
    klog("PANIC: stack canary corruption detected");
    serial_write_buf("\n*** stack smashing detected ***\n", 33);
    terminal_writestring("\n*** stack smashing detected ***\nSystem halted.\n");
    for (;;) __asm__ __volatile__("hlt");
}
