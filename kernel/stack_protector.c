/*
 * Freestanding runtime support for -fstack-protector-strong.
 *
 * GCC emits, in the prologue of every protected function, a copy of the global
 * __stack_chk_guard just past the saved return address; the epilogue compares
 * it and calls __stack_chk_fail() on mismatch. We must provide both symbols
 * ourselves since this is a freestanding kernel with no libc.
 */
#include "include/kernel/serial.h"
#include "include/kernel/rtc.h"
#include <stdint.h>

/* Nonzero default so canaries are meaningful even before reseeding. Overwritten
 * by stack_protector_init() early in boot with a per-run value. */
uintptr_t __stack_chk_guard = 0xE2DEE396u;

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Reseed the guard from the timestamp counter mixed with the RTC clock, so it
 * differs run-to-run. Marked no_stack_protector: it rewrites the very guard a
 * protected epilogue would check, which would false-trip if this function were
 * itself protected. Must be called before any protected frame that later
 * returns is still live — kernel_main calls it first and never returns.
 */
__attribute__((no_stack_protector))
void stack_protector_init(void) {
    uint64_t tsc = read_tsc();
    uint32_t secs = rtc_get_wallclock_seconds();
    uintptr_t guard = (uintptr_t)(tsc ^ (tsc >> 32)) ^ (secs * 2654435761u);
    /* Keep a NUL byte in the low position: string-based overflows that copy up
     * to a terminator then can't forge the full guard. Also guarantee nonzero. */
    guard &= 0xFFFFFF00u;
    if (guard == 0u) guard = 0xDEAD0000u;
    __stack_chk_guard = guard;
}

__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void) {
    const char msg[] = "\n!!! stack smashing detected — kernel halted\n";
    serial_write_buf(msg, sizeof(msg) - 1u);
    __asm__ __volatile__("cli");
    for (;;) __asm__ __volatile__("hlt");
}
