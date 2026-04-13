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
14. ~~Virtual memory features: demand paging hooks, copy‑on‑write for fork (later).~~
15. ~~Process management enhancements: parent/child, signals or simple kill mechanism.~~
16. ~~Fork groundwork: duplicate task context and memory metadata without full copy.~~
17. ~~Execve path: replace current process image with a new ELF while preserving PID.~~
18. ~~Wait/waitpid: let parents block for child exit and collect status codes.~~
19. ~~File descriptor table: per-process fd state with close-on-exec support.~~
20. ~~Pipes: anonymous unidirectional pipes for shell command chaining.~~
21. ~~Redirection: shell support for >, >>, < with robust fd wiring.~~
22. ~~Session/job control basics: foreground/background groups and terminal ownership.~~
23. ~~Signal delivery core: SIGINT/SIGTERM/SIGKILL/SIGCHLD semantics and default handlers.~~
24. ~~Signal APIs: sigaction/signal-style user API and kernel dispatch hooks.~~
25. ~~Copy-on-write completion: full fork COW for user pages with verified refcount correctness.~~
26. ~~Demand paging completion: lazily load ELF text/data pages on first access.~~
27. ~~mmap/munmap primitives: map anonymous and file-backed regions.~~
28. ~~Shared memory: lightweight shm API for IPC between processes.~~
29. ~~Swap design pass: plan paging-to-disk layout and eviction policy.~~
30. ~~Swap implementation: page-out/page-in path with fault retry logic.~~
31. ~~VFS cleanup: unify mount points, path normalization, and permission checks.~~
32. ~~Real filesystem phase 1: FAT32 read-only mount support for disk partitions.~~
33. ~~Real filesystem phase 2: FAT32 write support with crash-safe metadata updates.~~
34. ~~Filesystem cache: block cache with LRU/clock eviction and dirty writeback.~~
35. ~~Journaling/minfs reliability: add transaction log or recovery markers.~~
36. ~~Unified storage probing: robust ATA + ATAPI + partition scan on boot.~~
37. ~~Better block I/O APIs: async request queue and completion callbacks.~~
38. ~~Network driver milestone: bring up a basic NIC (e1000 or RTL8139).~~
39. ~~Network stack phase 1: ARP + IPv4 + ICMP (ping).~~
40. ~~Network stack phase 2: UDP sockets and DNS client.~~
41. ~~Network stack phase 3: TCP state machine for simple clients.~~
42. ~~Userland networking tools: ping, netstat-lite, and DHCP client command.~~
43. ~~Security baseline: user/group IDs, chmod/chown, and permission enforcement.~~
44. Hardening: kernel/user pointer validation audit for all syscalls.
45. Hardening: stack canary support and non-executable user stacks.
46. Randomness: entropy collector + /dev/random and /dev/urandom devices.
47. Timekeeping improvements: monotonic clock + RTC synchronization strategy.
48. TTY improvements: line discipline, canonical/raw mode, and control characters.
49. Terminal UX: command history persistence and reverse search.
50. Shell scripting mini-language: variables, simple loops, and conditionals.
51. Boot reliability: support booting cleanly from disk without ISO fallback.
52. SMP research task: AP bootstrap plan and per-CPU data structures.
53. SMP phase 1: bring up second core and schedule idle task there.
54. Locking audit: spinlocks/mutexes around scheduler, VFS, and heap critical paths.
55. Deadlock diagnostics: lock order tracking and watchdog warnings.
56. Test framework expansion: kernel unit tests + integration tests in QEMU.
57. Regression suite: scripted boot tests for shell, FS, exec, and block I/O.
58. Fuzzing pass: syscall and filesystem input fuzz tests.
59. Profiling support: PIT-based sampling profiler and symbolized reports.
60. Panic diagnostics: richer crash dump with registers, stack trace, and task info.
61. Build system cleanup: reproducible builds and artifact version stamping.
62. CI pipeline: automated build + run + smoke tests on every change.
63. Developer docs: architecture overview and subsystem ownership notes.
64. Contributor guide: coding style, testing requirements, and review checklist.
65. User docs: shell command reference and filesystem usage guide.
66. Release process: changelog generation and semantic versioning policy.
67. Package format idea: simple app bundle with metadata and signatures.
68. User program set: coreutils-lite (cat, cp, mv, grep, hexdump).
69. Text editor prototype: minimal full-screen editor for in-OS editing.
70. Init system: configurable startup scripts and service supervision.
71. Power management: APM/ACPI detection and clean shutdown/reboot paths.
72. Long-run soak testing: overnight stress tests for scheduler, memory, and I/O.
73. Performance tuning: benchmark suite and top bottleneck fixes.
74. Milestone release: freeze features, stabilize, and publish 1.0.0 roadmap.
