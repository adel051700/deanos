# I/O Systems
1. ~~Keyboard Driver: Implement PS/2 keyboard interrupt handling and input buffer~~
2. ~~Basic Shell: Create a command-line interface to interact with your OS~~
3. ~~File System: Implement a basic in-memory filesystem or read from disk~~
4. Optional: make rm spawn a task that overwrites the file with random data before deleting it

# Memory Management
1. Paging Setup: Implement proper memory paging to enable virtual memory
2. Heap Allocation: Create a heap memory manager with malloc/free functions
3. Memory Map: Parse the multiboot memory map to know available RAM regions

# Process Management
~~1. Basic Task Switching: Implement context switching between simple tasks~~
~~2. Simple Scheduler: Create a round-robin scheduler to run multiple processes~~
~~3. User Space: Set up user mode and privilege separation from kernel~~

# System Services
1. ~~System Clock: Set up the PIT (Programmable Interval Timer) for timing~~
2. ~~Interrupt Handling: Improve your IDT (Interrupt Descriptor Table) setup~~
3. System Calls: Create an interface for user programs to request kernel services

# Development Tools
1. Debugging: Add better debug output capabilities
2. CPUID Detection: Identify CPU features at runtime
3. Unit Testing: Build a test framework for kernel components

# In Order do these:
1. ~~Physical memory management: parse multiboot memory map, build bitmap / buddy frame allocator.~~
2. ~~Paging + page fault handler: enable 32‑bit paging, identity map kernel, map heap region.~~
3. ~~Kernel heap: simple malloc/free (e.g. dlmalloc or a slab/free‑list) atop frame allocator.~~
4. ~~Better interrupt/IRQ layering: separate PIC + ISR dispatch, add simple logging.~~
5. ~~System call interface: syscall gate (int 0x80 / 0x81) + minimal API (write, time, exit)~~.
6. ~~Task switching: struct task (regs, stacks), round‑robin scheduler driven by PIT.~~
7. ~~User mode transition: create ring3 code/data segments, switch via iret, run a test user task.~~
8. ~~Basic VFS abstraction: in‑memory ramfs with open/read/write/stat; later real disk driver.~~
9. ~~ELF loader: load a user program into its address space, set up stack, start in user mode.~~
10. ~~Simple libc expansion: printf, malloc (once heap ready), basic file I/O wrappers.~~
11. ~~Timer improvements: expose sleep/yield; convert busy waits to scheduler blocks.~~
12. ~~Logging & debug: kernel log buffer, serial port driver (COM1) for debug output.~~
13. ~~Driver work: PS/2 mouse, ATA/ATAPI (PIO first), then a block device layer.~~
14. Virtual memory features: demand paging hooks, copy‑on‑write for fork (later).
15. Process management enhancements: parent/child, signals or simple kill mechanism.