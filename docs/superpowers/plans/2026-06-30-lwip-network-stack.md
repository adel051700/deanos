# lwIP Network Stack Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace deanos's hand-rolled network stack (`net.c`/`dns.c`/`dhcp.c`) with lwIP in `NO_SYS=1` (raw API) mode, keeping the existing e1000/rtl8139 NIC drivers and cleaning the userland socket ABI toward standard fd-based BSD sockets.

**Architecture:** lwIP is vendored as a git submodule and compiled into the freestanding `-m32` kernel. A thin port layer (`kernel/lwip_port/`) provides `lwipopts.h`, `arch/cc.h`, `sys_now()`, a static lwIP heap, and a `netif` glue that bridges the drivers' `*_send_raw` / `*_set_rx_callback` hooks to lwIP. A single `net_service_tick()` pumps deferred RX and `sys_check_timeouts()` from the scheduler loop. The kernel's `syscall.c` socket layer drives lwIP's raw TCP/UDP API directly (no lwIP socket/netconn layer), implementing blocking via `net_service_tick()` + `task_yield()`.

**Tech Stack:** lwIP STABLE-2_2_0, i686-elf-gcc cross toolchain, freestanding C, QEMU (`-netdev user`) for verification, GRUB ISO.

## Global Constraints

- Cross compiler: `/home/adel/opt/cross/bin/i686-elf-gcc` (and `-as`, `-ld`). Target i386, little-endian.
- All kernel objects compile with: `-O2 -g -ffreestanding -Wall -Wextra -fno-pie -fno-stack-protector -Ikernel/include -Ilibc/include` (plus new lwIP includes). No hosted libc, no stack protector, no PIE.
- lwIP config: `NO_SYS=1`, `SYS_LIGHTWEIGHT_PROT=0`, `LWIP_NETCONN=0`, `LWIP_SOCKET=0`, `MEM_LIBC_MALLOC=0` (static lwIP heap), IPv6 off.
- lwIP submodule path: `third_party/lwip/`, pinned to tag `STABLE-2_2_0`.
- Port layer path: `kernel/lwip_port/`.
- **No host unit-test harness exists** in this repo (todo.md item 56 is open). "Tests" in this plan are: (a) the kernel **builds clean** (`make` with no new warnings/errors), and (b) **QEMU smoke runs** show expected on-screen/serial output. Each task states the exact build/run command and expected observation. Treat a failing build or missing observation as a red test.
- Verification QEMU invocations already exist: `make run-net` (e1000) and `make run-net-rtl` (rtl8139).
- Frequent commits: one commit per task minimum, message prefix `feat(net):` / `build(net):` / `refactor(net):` / `chore(net):`.
- **Migration safety:** the kernel must build and boot after every task. The old stack is kept compiling behind a `USE_LWIP` switch until Task 14 deletes it.

---

## File Structure

**Created:**
- `third_party/lwip/` — submodule (upstream lwIP).
- `kernel/lwip_port/lwipopts.h` — lwIP feature/heap config.
- `kernel/lwip_port/arch/cc.h` — compiler/platform glue.
- `kernel/lwip_port/lwip_glue.c` — `sys_now()`, lwIP heap-less assert/diag hooks, `net_lwip_*` init entry.
- `kernel/lwip_port/deanos_netif.c` — netif TX/RX glue + RX defer queue.
- `kernel/lwip_port/deanos_netif.h` — netif glue public API.
- `kernel/include/kernel/net_lwip.h` — trimmed public network API used by `kernel.c`, `syscall.c`, `shell.c`.

**Modified:**
- `Makefile` — lwIP source list, include paths, build dir.
- `.github/workflows/*` — submodule checkout before build.
- `.gitignore` — `build/lwip/`.
- `kernel/e1000.c`, `kernel/rtl8139.c` — (no logic change) confirm `set_rx_callback`/`send_raw`/`get_mac` used by glue.
- `kernel/kernel.c:55` — call new init.
- `kernel/syscall.c` — socket node ops re-pointed at lwIP raw API; ABI cleanup.
- `kernel/shell.c` — `net` command reworked onto lwIP; ABI-updated socket calls.
- `libc/include/sys/socket.h`, `libc/unistd/syscalls.c`, `libc/netdb/resolve.c` — ABI cleanup.

**Deleted (Task 14):**
- `kernel/net.c`, `kernel/dns.c`, `kernel/dhcp.c`, `kernel/include/kernel/net.h`, `kernel/include/kernel/net_dns.h`, `kernel/include/kernel/net_dhcp.h` (whichever become unused).

---

## Task 1: Vendor lwIP as a submodule

**Files:**
- Create: `third_party/lwip/` (submodule)
- Modify: `.gitmodules` (auto), `.gitignore`

**Interfaces:**
- Produces: lwIP source tree at `third_party/lwip/src/` with `Filelists.mk`, headers under `third_party/lwip/src/include/`.

- [ ] **Step 1: Add the submodule pinned to the stable tag**

```bash
cd /home/adel/desktop/Uni/free_time/deanos
git submodule add https://github.com/lwip-tcpip/lwip.git third_party/lwip
git -C third_party/lwip fetch --tags
git -C third_party/lwip checkout STABLE-2_2_0
git add third_party/lwip .gitmodules
```

- [ ] **Step 2: Verify the expected files exist**

Run:
```bash
ls third_party/lwip/src/Filelists.mk third_party/lwip/src/include/lwip/init.h && git -C third_party/lwip describe --tags
```
Expected: both paths listed, and `describe` prints `STABLE-2_2_0`.

- [ ] **Step 3: Ignore lwIP build output**

Add to `.gitignore`:
```
build/lwip/
```

- [ ] **Step 4: Commit**

```bash
git add .gitignore .gitmodules third_party/lwip
git commit -m "build(net): vendor lwIP STABLE-2_2_0 as submodule"
```

---

## Task 2: Port-layer headers (`lwipopts.h`, `cc.h`)

These let lwIP **compile** against the freestanding toolchain. No driver yet.

**Files:**
- Create: `kernel/lwip_port/lwipopts.h`
- Create: `kernel/lwip_port/arch/cc.h`

**Interfaces:**
- Produces: a config header `lwipopts.h` and `arch/cc.h` that satisfy `#include "lwip/opt.h"`.

- [ ] **Step 1: Write `kernel/lwip_port/arch/cc.h`**

```c
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

/* i386 is little-endian; lwIP provides htons/htonl from this. */
#define BYTE_ORDER LITTLE_ENDIAN

/* Struct packing for protocol headers (GCC). */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

/* Diagnostics / asserts route into the kernel log + panic. */
void lwip_port_diag(const char* fmt, ...);
void lwip_port_assert_fail(const char* msg, const char* file, int line);

#define LWIP_PLATFORM_DIAG(x)   do { lwip_port_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { lwip_port_assert_fail((x), __FILE__, __LINE__); } while (0)

/* No errno.h in this freestanding libc: let lwIP define its own. */
#define LWIP_PROVIDE_ERRNO 1

#endif /* LWIP_ARCH_CC_H */
```

- [ ] **Step 2: Write `kernel/lwip_port/lwipopts.h`**

```c
#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* ---- Core mode: bare-metal, single-threaded raw API ---- */
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0   /* no sequential API */
#define LWIP_SOCKET                 0   /* BSD layer implemented by syscall.c */
#define LWIP_NETIF_API              0

/* ---- Memory: static lwIP heap, isolated from kernel kheap ---- */
#define MEM_LIBC_MALLOC             0
#define MEMP_MEM_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (64 * 1024)

#define MEMP_NUM_PBUF               32
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_PCB            12
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          16
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)
#define PBUF_POOL_SIZE              48
#define PBUF_POOL_BUFSIZE           1536

/* ---- Protocols ---- */
#define LWIP_ARP                    1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

/* DHCP needs ARP check disabled to be simple; allow broadcast. */
#define LWIP_DHCP_DOES_ACD_CHECK    0

/* ---- TCP tuning (small footprint) ---- */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define LWIP_TCP_KEEPALIVE          1

/* ---- Checksums computed in software (no NIC offload) ---- */
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_ICMP         1

/* ---- Stats / debug (off by default; flip on locally when needed) ---- */
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

/* No OS threads to defer into. */
#define LWIP_TCPIP_CORE_LOCKING     0

#endif /* LWIP_LWIPOPTS_H */
```

- [ ] **Step 3: Sanity-check the config compiles against lwIP's opt.h**

Run:
```bash
/home/adel/opt/cross/bin/i686-elf-gcc -ffreestanding -fno-pie -fno-stack-protector \
  -Ikernel/include -Ilibc/include \
  -Ikernel/lwip_port -Ithird_party/lwip/src/include \
  -c third_party/lwip/src/core/init.c -o /tmp/lwip_init_probe.o
```
Expected: compiles to `/tmp/lwip_init_probe.o` with no errors (warnings about unused are acceptable). If it fails on a missing macro, add the macro to `lwipopts.h` and re-run.

- [ ] **Step 4: Commit**

```bash
git add kernel/lwip_port/lwipopts.h kernel/lwip_port/arch/cc.h
git commit -m "feat(net): add lwIP port config (lwipopts.h, cc.h)"
```

---

## Task 3: Build lwIP into the kernel (stub, no netif)

Get the whole lwIP core compiling and **linking** inside the kernel image, with the diag/assert hooks and `sys_now()` defined. Still no driver wired in.

**Files:**
- Create: `kernel/lwip_port/lwip_glue.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: `pit_get_ms()` (existing PIT millisecond counter — confirm exact name in Step 1), `klog`/serial print (existing), `panic()` (existing).
- Produces: `u32_t sys_now(void)`, `void lwip_port_diag(const char*, ...)`, `void lwip_port_assert_fail(...)`, `void net_lwip_core_init(void)` (calls `lwip_init()`).

- [ ] **Step 1: Confirm the PIT millisecond and log/panic symbols**

Run:
```bash
grep -rnE "uint32_t .*(ms|millis|ticks)|void panic|klog|serial_write" kernel/include/kernel/pit.h kernel/include/kernel/log.h kernel/include/kernel/serial.h
```
Expected: note the exact function returning elapsed milliseconds (e.g. `pit_get_ms`/`pit_millis`/`pit_ticks`*tick_ms) and the log/panic entry points. Use those exact names in Step 2 (replace `pit_get_ms()`/`klog_printf`/`panic` below if they differ).

- [ ] **Step 2: Write `kernel/lwip_port/lwip_glue.c`**

```c
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "arch/cc.h"
#include "include/kernel/pit.h"
#include "include/kernel/log.h"
#include <stdarg.h>
#include <stdint.h>

/* milliseconds since boot for lwIP timeouts */
u32_t sys_now(void) {
    return (u32_t)pit_get_ms();   /* adjust to the confirmed symbol */
}

void lwip_port_diag(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    klog_vprintf(fmt, ap);        /* adjust to the confirmed vprintf-style logger */
    va_end(ap);
}

void lwip_port_assert_fail(const char* msg, const char* file, int line) {
    klog_printf("lwIP assert: %s at %s:%d\n", msg, file, line);
    panic("lwIP assertion failed");
}

void net_lwip_core_init(void) {
    lwip_init();
}
```

If `klog_vprintf` does not exist, format into a stack buffer with the existing `printf`-family and call the available logger; do not invent a new logger.

- [ ] **Step 3: Add lwIP sources + glue to the Makefile**

In `Makefile`, after the `CPPFLAGS` line, add lwIP include dirs:
```make
CPPFLAGS:=$(CPPFLAGS) -Ikernel/lwip_port -Ithird_party/lwip/src/include
```

Add an lwIP build dir and source list. After `USER_BUILD_DIR = $(BUILD_DIR)/user` add:
```make
LWIP_BUILD_DIR = $(BUILD_DIR)/lwip
LWIP_DIR = third_party/lwip
include $(LWIP_DIR)/src/Filelists.mk
# Filelists.mk exports COREFILES CORE4FILES NETIFFILES (and others) as
# space-separated absolute-ish paths under $(LWIP_DIR)/src/...
LWIP_SRCS = $(COREFILES) $(CORE4FILES) $(NETIFFILES)
LWIP_PORT_SRCS = kernel/lwip_port/lwip_glue.c kernel/lwip_port/deanos_netif.c
LWIP_OBJS = $(patsubst $(LWIP_DIR)/src/%.c,$(LWIP_BUILD_DIR)/%.o,$(LWIP_SRCS))
LWIP_OBJS += $(patsubst kernel/lwip_port/%.c,$(LWIP_BUILD_DIR)/port/%.o,$(LWIP_PORT_SRCS))
```

> Note: `deanos_netif.c` is referenced here but created in Task 4. Until then, comment out the `deanos_netif.c` entry in `LWIP_PORT_SRCS` so this task links; re-enable it in Task 4.

Add compile rules (near the other `%.o` rules):
```make
$(LWIP_BUILD_DIR)/%.o: $(LWIP_DIR)/src/%.c | directories
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS) -Wno-unused-parameter -Wno-address

$(LWIP_BUILD_DIR)/port/%.o: kernel/lwip_port/%.c | directories
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS) -Wno-unused-parameter
```

Add `$(LWIP_OBJS)` to the final link list:
```make
ALL_OBJS = $(ARCH_BUILD_DIR)/boot/boot.o $(ARCH_BUILD_DIR)/interrupt.o $(ARCH_BUILD_DIR)/gdt.o $(ARCH_C_OBJS) $(KERNEL_OBJS) $(LWIP_OBJS) $(LIBC_OBJS) $(USER_BLOB_OBJS)
```

Add a `mkdir` for the lwIP build dir in the `directories:` target:
```make
	@mkdir -p $(LWIP_BUILD_DIR)
```

- [ ] **Step 4: Build the kernel and confirm lwIP links**

Run:
```bash
make clean && make 2>&1 | tail -30
```
Expected: build reaches the `deanos.bin` link step and prints `✓ Multiboot2 header found!`. lwIP `.o` files appear under `build/lwip/`. No undefined-reference errors for `lwip_init`, `sys_now`. (`net_lwip_core_init` may be unused-but-defined — fine.)

- [ ] **Step 5: Commit**

```bash
git add Makefile kernel/lwip_port/lwip_glue.c
git commit -m "build(net): compile and link lwIP core into kernel (no netif yet)"
```

---

## Task 4: netif glue + deferred RX (`deanos_netif.c`)

Bridge the drivers to a lwIP `netif`: TX out via `*_send_raw`, RX in via the driver callback → defer queue → `netif->input`.

**Files:**
- Create: `kernel/lwip_port/deanos_netif.h`
- Create: `kernel/lwip_port/deanos_netif.c`
- Modify: `Makefile` (re-enable `deanos_netif.c` from Task 3 Step 3)

**Interfaces:**
- Consumes (existing, confirmed in Task 0 exploration): `int e1000_send_raw(const void*, uint16_t)`, `void e1000_set_rx_callback(void(*)(const uint8_t*, uint16_t))`, `void e1000_get_mac(uint8_t[6])`, `int e1000_link_up(void)`, `int e1000_is_ready(void)`, and the `rtl8139_*` equivalents. Selection of which driver is active is decided by the existing init code (`net_initialize` chose one; replicate that probe order).
- Produces:
  - `err_t deanos_netif_init(struct netif* netif)` — lwIP `netif_add` init callback.
  - `void net_lwip_rx_pump(void)` — drains the RX defer queue into `netif->input` (called by `net_service_tick`).
  - `struct netif* deanos_netif_default(void)` — pointer to the bound netif.
  - `int deanos_netif_bind_driver(void)` — probes/selects e1000 or rtl8139, registers the RX callback. Returns 0 on success, <0 if no NIC.

- [ ] **Step 1: Write `kernel/lwip_port/deanos_netif.h`**

```c
#ifndef DEANOS_NETIF_H
#define DEANOS_NETIF_H

#include "lwip/netif.h"
#include "lwip/err.h"

err_t deanos_netif_init(struct netif* netif);
int   deanos_netif_bind_driver(void);   /* 0 ok, <0 no NIC */
void  net_lwip_rx_pump(void);
struct netif* deanos_netif_default(void);
const char* deanos_netif_driver_name(void);

#endif
```

- [ ] **Step 2: Write `kernel/lwip_port/deanos_netif.c`**

```c
#include "deanos_netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "netif/ethernet.h"
#include "include/kernel/e1000.h"
#include "include/kernel/rtl8139.h"
#include "include/kernel/interrupt.h"   /* irq save/restore */
#include <string.h>
#include <stdint.h>

#define RX_QUEUE_LEN   64u
#define RX_FRAME_MAX   1600u

typedef struct rx_slot { uint16_t len; uint8_t data[RX_FRAME_MAX]; } rx_slot_t;

static rx_slot_t   g_rx_ring[RX_QUEUE_LEN];
static volatile uint32_t g_rx_head = 0, g_rx_tail = 0;   /* head=produce, tail=consume */
static struct netif g_netif;

static int (*g_tx)(const void*, uint16_t) = 0;
static void (*g_set_rx_cb)(void(*)(const uint8_t*, uint16_t)) = 0;
static void (*g_get_mac)(uint8_t[6]) = 0;
static int (*g_link_up)(void) = 0;
static const char* g_drv_name = "none";

/* IRQ context: copy frame into ring (drop if full). */
static void rx_isr_cb(const uint8_t* frame, uint16_t len) {
    uint32_t next;
    if (len > RX_FRAME_MAX) return;
    next = (g_rx_head + 1u) % RX_QUEUE_LEN;
    if (next == g_rx_tail) return;            /* full: drop */
    g_rx_ring[g_rx_head].len = len;
    memcpy(g_rx_ring[g_rx_head].data, frame, len);
    g_rx_head = next;
}

/* lwIP -> driver TX: flatten pbuf chain, send raw frame. */
static err_t netif_linkoutput(struct netif* netif, struct pbuf* p) {
    static uint8_t txbuf[RX_FRAME_MAX];
    uint16_t off = 0;
    struct pbuf* q;
    (void)netif;
    if (p->tot_len > RX_FRAME_MAX) return ERR_BUF;
    for (q = p; q != NULL; q = q->next) {
        memcpy(txbuf + off, q->payload, q->len);
        off += q->len;
    }
    if (g_tx && g_tx(txbuf, off) == 0) return ERR_OK;
    return ERR_IF;
}

err_t deanos_netif_init(struct netif* netif) {
    netif->name[0] = 'e'; netif->name[1] = 'n';
    netif->output = etharp_output;          /* ARP for IPv4 */
    netif->linkoutput = netif_linkoutput;
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    if (g_get_mac) g_get_mac(netif->hwaddr);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

void net_lwip_rx_pump(void) {
    while (g_rx_tail != g_rx_head) {
        rx_slot_t* s = &g_rx_ring[g_rx_tail];
        struct pbuf* p = pbuf_alloc(PBUF_RAW, s->len, PBUF_POOL);
        if (p) {
            pbuf_take(p, s->data, s->len);
            if (g_netif.input(p, &g_netif) != ERR_OK) pbuf_free(p);
        }
        g_rx_tail = (g_rx_tail + 1u) % RX_QUEUE_LEN;
    }
}

int deanos_netif_bind_driver(void) {
    /* Mirror the old net_initialize probe order: e1000 first, then rtl8139. */
    if (e1000_is_ready()) {
        g_tx = e1000_send_raw; g_set_rx_cb = e1000_set_rx_callback;
        g_get_mac = e1000_get_mac; g_link_up = e1000_link_up; g_drv_name = "e1000";
    } else if (rtl8139_is_ready()) {
        g_tx = rtl8139_send_raw; g_set_rx_cb = rtl8139_set_rx_callback;
        g_get_mac = rtl8139_get_mac; g_link_up = rtl8139_link_up; g_drv_name = "rtl8139";
    } else {
        return -1;
    }
    g_set_rx_cb(rx_isr_cb);
    return 0;
}

struct netif* deanos_netif_default(void) { return &g_netif; }
const char* deanos_netif_driver_name(void) { return g_drv_name; }
```

> If the driver `is_ready`/`get_mac`/`link_up` symbol names differ, run `grep -nE "e1000_(is_ready|get_mac|link_up|send_raw|set_rx_callback)" kernel/include/kernel/e1000.h` and use the exact names. The `interrupt.h` include is only needed if you add IRQ-disable around the ring; the single-producer/single-consumer ring above is safe without it on a uniprocessor as long as head/tail are `volatile` and updated last.

- [ ] **Step 3: Re-enable `deanos_netif.c` in the Makefile**

Uncomment / restore the `deanos_netif.c` entry in `LWIP_PORT_SRCS` (from Task 3 Step 3).

- [ ] **Step 4: Build**

Run:
```bash
make 2>&1 | tail -20
```
Expected: links to `deanos.bin`, `✓ Multiboot2 header found!`. Functions `deanos_netif_init`, `net_lwip_rx_pump`, `deanos_netif_bind_driver` defined-but-possibly-unused (fine until Task 5).

- [ ] **Step 5: Commit**

```bash
git add kernel/lwip_port/deanos_netif.c kernel/lwip_port/deanos_netif.h Makefile
git commit -m "feat(net): add lwIP netif glue with deferred RX into netif->input"
```

---

## Task 5: Bring the interface up + DHCP + service tick (ping works)

Wire init: bind driver → `netif_add` → `netif_set_default`/`up` → `dhcp_start`. Add `net_service_tick()` and call it from the scheduler/idle loop and from the existing poll path. This is the first end-to-end milestone: **ICMP ping out works under QEMU.**

**Files:**
- Create: `kernel/include/kernel/net_lwip.h`
- Modify: `kernel/lwip_port/lwip_glue.c` (add `net_lwip_start`, `net_service_tick`)
- Modify: `kernel/kernel.c:55` (call `net_lwip_start` instead of `net_initialize`)
- Modify: the scheduler/idle loop (confirm location in Step 1) to call `net_service_tick()`

**Interfaces:**
- Consumes: `deanos_netif_bind_driver()`, `deanos_netif_init()`, `deanos_netif_default()`, `net_lwip_rx_pump()`, `sys_check_timeouts()`, lwIP `netif_add`/`netif_set_default`/`netif_set_up`/`dhcp_start`.
- Produces:
  - `int  net_lwip_start(void)` — full bring-up; returns 0 ok / <0 no NIC.
  - `void net_service_tick(void)` — `net_lwip_rx_pump()` + `sys_check_timeouts()`.
  - `int  net_lwip_is_ready(void)` — link/driver bound.
  - `void net_lwip_get_ipv4(uint8_t out[4])`, `..._netmask`, `..._gateway`, `void net_lwip_get_mac(uint8_t out[6])`, `const char* net_lwip_driver_name(void)`.

- [ ] **Step 1: Find the idle/scheduler loop and the existing poll site**

Run:
```bash
grep -rnE "net_poll|idle|schedule|hlt|while *\(1\)" kernel/task.c kernel/kernel.c kernel/syscall.c | head
```
Expected: identify (a) the idle loop or main kernel loop where `net_service_tick()` should be pumped each iteration, and (b) `kernel/syscall.c:388` where `net_poll(0u)` is called inside `sys_poll` (replace with `net_service_tick()`).

- [ ] **Step 2: Write `kernel/include/kernel/net_lwip.h`**

```c
#ifndef _KERNEL_NET_LWIP_H
#define _KERNEL_NET_LWIP_H
#include <stdint.h>

int  net_lwip_start(void);
void net_service_tick(void);
int  net_lwip_is_ready(void);
const char* net_lwip_driver_name(void);

void net_lwip_get_mac(uint8_t out_mac[6]);
void net_lwip_get_ipv4(uint8_t out_ip[4]);
void net_lwip_get_ipv4_netmask(uint8_t out_mask[4]);
void net_lwip_get_ipv4_gateway(uint8_t out_gw[4]);

#endif
```

- [ ] **Step 3: Implement bring-up + tick in `lwip_glue.c`**

Append to `kernel/lwip_port/lwip_glue.c`:
```c
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip_addr.h"
#include "deanos_netif.h"
#include "include/kernel/net_lwip.h"

static int g_started = 0;

int net_lwip_start(void) {
    ip4_addr_t any; ip4_addr_set_zero(&any);
    lwip_init();
    if (deanos_netif_bind_driver() != 0) return -1;
    netif_add(deanos_netif_default(), &any, &any, &any, NULL,
              deanos_netif_init, netif_input);
    netif_set_default(deanos_netif_default());
    netif_set_up(deanos_netif_default());
    netif_set_link_up(deanos_netif_default());
    dhcp_start(deanos_netif_default());
    g_started = 1;
    return 0;
}

void net_service_tick(void) {
    if (!g_started) return;
    net_lwip_rx_pump();
    sys_check_timeouts();
}

int net_lwip_is_ready(void) {
    return g_started && netif_is_up(deanos_netif_default());
}
const char* net_lwip_driver_name(void) { return deanos_netif_driver_name(); }

static void copy_ip4(uint8_t out[4], const ip4_addr_t* a) {
    uint32_t v = ip4_addr_get_u32(a);
    out[0]=(uint8_t)(v); out[1]=(uint8_t)(v>>8); out[2]=(uint8_t)(v>>16); out[3]=(uint8_t)(v>>24);
}
void net_lwip_get_ipv4(uint8_t o[4])         { copy_ip4(o, netif_ip4_addr(deanos_netif_default())); }
void net_lwip_get_ipv4_netmask(uint8_t o[4]) { copy_ip4(o, netif_ip4_netmask(deanos_netif_default())); }
void net_lwip_get_ipv4_gateway(uint8_t o[4]) { copy_ip4(o, netif_ip4_gw(deanos_netif_default())); }
void net_lwip_get_mac(uint8_t o[6])          { memcpy(o, deanos_netif_default()->hwaddr, 6); }
```

- [ ] **Step 4: Call new init from `kernel.c`**

In `kernel/kernel.c` around line 55, replace `(void)net_initialize();` with:
```c
(void)net_lwip_start();
```
Add `#include "include/kernel/net_lwip.h"` near the other includes.

- [ ] **Step 5: Pump the tick from the idle loop and the poll site**

In the idle/main loop found in Step 1, add (once per loop iteration, before `hlt`/yield):
```c
net_service_tick();
```
In `kernel/syscall.c:388`, replace `(void)net_poll(0u);` with `net_service_tick();` and add `#include "include/kernel/net_lwip.h"`.

> Leave the old `net.c` still compiled for now (other `net_*` callers in `shell.c`/`syscall.c` still reference it). Both stacks linked at once is fine as long as only one binds the driver's RX callback — and only lwIP does (`net_lwip_start` registers `rx_isr_cb`; do **not** also call the old `net_initialize`). This is the temporary `USE_LWIP` state.

- [ ] **Step 6: Build and run under QEMU; verify ping**

Run:
```bash
make run-net
```
In the deanos shell, run the existing ping command against the QEMU gateway:
```
net ping 10.0.2.2
```
Expected: at least one ICMP echo reply reported (QEMU's user-net gateway is `10.0.2.2`). Also confirm `net ip` (or `net`) shows a DHCP-assigned address in `10.0.2.x` with gateway `10.0.2.2`.

> If ping still routes through old `net.c` code (it will until Task 9 rewires the shell), instead verify lwIP itself by temporarily adding a one-line debug in `net_service_tick` or checking that `dhcp` assigned an address via lwIP. The authoritative ping-over-lwIP check happens in Task 9; for this task, success = DHCP lease obtained via lwIP (address shows in `10.0.2.15` range) and no crash.

- [ ] **Step 7: Commit**

```bash
git add kernel/lwip_port/lwip_glue.c kernel/include/kernel/net_lwip.h kernel/kernel.c kernel/syscall.c kernel/task.c
git commit -m "feat(net): bring lwIP netif up with DHCP and service tick"
```

---

## Task 6: UDP socket backend on lwIP raw API

Replace the kernel-side UDP socket implementation used by `syscall.c` with lwIP raw UDP, behind a small adapter that keeps the `ksock` node model.

**Files:**
- Create: `kernel/lwip_port/ksock_udp.c`
- Create: `kernel/lwip_port/ksock_udp.h`
- Modify: `Makefile` (`LWIP_PORT_SRCS += ksock_udp.c`)

**Interfaces:**
- Consumes: lwIP `udp_new`, `udp_bind`, `udp_recv`, `udp_sendto`, `udp_remove`, `pbuf_alloc`/`pbuf_take`/`pbuf_free`, `net_service_tick`, `task_yield` (confirm exact name in Step 1).
- Produces:
  - `typedef struct ksock_udp ksock_udp_t;`
  - `ksock_udp_t* ksock_udp_open(void);`
  - `int ksock_udp_bind(ksock_udp_t*, uint16_t local_port);`
  - `int ksock_udp_sendto(ksock_udp_t*, const uint8_t ip[4], uint16_t port, const void* buf, uint16_t len);`
  - `int ksock_udp_recvfrom(ksock_udp_t*, void* buf, uint16_t cap, uint16_t* out_len, uint8_t out_ip[4], uint16_t* out_port, uint32_t timeout_ms, int nonblock);` — returns bytes, 0 on timeout/would-block (nonblock), <0 error.
  - `int ksock_udp_readable(ksock_udp_t*);`
  - `void ksock_udp_close(ksock_udp_t*);`

- [ ] **Step 1: Confirm the yield/sleep primitive**

Run:
```bash
grep -rnE "void task_yield|task_sleep|void yield|pit_get_ms" kernel/include/kernel/task.h kernel/include/kernel/pit.h
```
Expected: note exact `task_yield()`/`task_sleep_ms()` and the ms counter; use them below.

- [ ] **Step 2: Write `kernel/lwip_port/ksock_udp.h`**

```c
#ifndef KSOCK_UDP_H
#define KSOCK_UDP_H
#include <stdint.h>

typedef struct ksock_udp ksock_udp_t;

ksock_udp_t* ksock_udp_open(void);
int  ksock_udp_bind(ksock_udp_t*, uint16_t local_port);
int  ksock_udp_sendto(ksock_udp_t*, const uint8_t ip[4], uint16_t port,
                      const void* buf, uint16_t len);
int  ksock_udp_recvfrom(ksock_udp_t*, void* buf, uint16_t cap, uint16_t* out_len,
                        uint8_t out_ip[4], uint16_t* out_port,
                        uint32_t timeout_ms, int nonblock);
int  ksock_udp_readable(ksock_udp_t*);
void ksock_udp_close(ksock_udp_t*);

#endif
```

- [ ] **Step 3: Write `kernel/lwip_port/ksock_udp.c`**

```c
#include "ksock_udp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "include/kernel/net_lwip.h"
#include "include/kernel/kheap.h"
#include "include/kernel/task.h"
#include "include/kernel/pit.h"
#include <string.h>

#define UDP_RXQ 16
typedef struct { uint8_t ip[4]; uint16_t port, len; uint8_t data[1472]; } udp_dgram_t;

struct ksock_udp {
    struct udp_pcb* pcb;
    udp_dgram_t q[UDP_RXQ];
    uint32_t head, tail;   /* head=produce (callback), tail=consume */
};

static void udp_rx_cb(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                      const ip_addr_t* addr, u16_t port) {
    struct ksock_udp* s = (struct ksock_udp*)arg;
    uint32_t next = (s->head + 1u) % UDP_RXQ;
    (void)pcb;
    if (p && next != s->tail && p->tot_len <= sizeof(s->q[0].data)) {
        udp_dgram_t* d = &s->q[s->head];
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(addr));
        d->ip[0]=(uint8_t)v; d->ip[1]=(uint8_t)(v>>8); d->ip[2]=(uint8_t)(v>>16); d->ip[3]=(uint8_t)(v>>24);
        d->port = port; d->len = p->tot_len;
        pbuf_copy_partial(p, d->data, p->tot_len, 0);
        s->head = next;
    }
    if (p) pbuf_free(p);
}

ksock_udp_t* ksock_udp_open(void) {
    struct ksock_udp* s = (struct ksock_udp*)kmalloc(sizeof(*s));
    if (!s) return 0;
    memset(s, 0, sizeof(*s));
    s->pcb = udp_new();
    if (!s->pcb) { kfree(s); return 0; }
    udp_recv(s->pcb, udp_rx_cb, s);
    return s;
}

int ksock_udp_bind(ksock_udp_t* s, uint16_t port) {
    if (!s) return -1;
    return udp_bind(s->pcb, IP_ANY_TYPE, port) == ERR_OK ? 0 : -1;
}

int ksock_udp_sendto(ksock_udp_t* s, const uint8_t ip[4], uint16_t port,
                     const void* buf, uint16_t len) {
    ip_addr_t dst; struct pbuf* p; err_t e;
    if (!s) return -1;
    IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);
    p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (!p) return -1;
    pbuf_take(p, buf, len);
    e = udp_sendto(s->pcb, p, &dst, port);
    pbuf_free(p);
    return e == ERR_OK ? (int)len : -1;
}

int ksock_udp_readable(ksock_udp_t* s) { return s && s->head != s->tail; }

int ksock_udp_recvfrom(ksock_udp_t* s, void* buf, uint16_t cap, uint16_t* out_len,
                       uint8_t out_ip[4], uint16_t* out_port,
                       uint32_t timeout_ms, int nonblock) {
    uint32_t start = (uint32_t)pit_get_ms();
    if (!s) return -1;
    for (;;) {
        if (s->head != s->tail) {
            udp_dgram_t* d = &s->q[s->tail];
            uint16_t n = d->len < cap ? d->len : cap;
            memcpy(buf, d->data, n);
            if (out_len) *out_len = n;
            if (out_ip) memcpy(out_ip, d->ip, 4);
            if (out_port) *out_port = d->port;
            s->tail = (s->tail + 1u) % UDP_RXQ;
            return (int)n;
        }
        if (nonblock) return 0;
        if (timeout_ms && (uint32_t)pit_get_ms() - start >= timeout_ms) return 0;
        net_service_tick();
        task_yield();
    }
}

void ksock_udp_close(ksock_udp_t* s) {
    if (!s) return;
    if (s->pcb) udp_remove(s->pcb);
    kfree(s);
}
```

- [ ] **Step 4: Add to Makefile and build**

Add `kernel/lwip_port/ksock_udp.c` to `LWIP_PORT_SRCS`. Run:
```bash
make 2>&1 | tail -20
```
Expected: clean link.

- [ ] **Step 5: Commit**

```bash
git add kernel/lwip_port/ksock_udp.c kernel/lwip_port/ksock_udp.h Makefile
git commit -m "feat(net): UDP socket backend on lwIP raw API"
```

---

## Task 7: DNS resolver on lwIP

Replace `net_dns_query_a` with a wrapper over lwIP's `dns_gethostbyname`, blocking via the tick loop.

**Files:**
- Create: `kernel/lwip_port/ksock_dns.c`
- Create: `kernel/lwip_port/ksock_dns.h`
- Modify: `Makefile`

**Interfaces:**
- Consumes: lwIP `dns_gethostbyname`, `dns_setserver`, `net_service_tick`, `task_yield`, `pit_get_ms`.
- Produces: `int ksock_dns_query_a(const char* host, uint8_t out_ip[4], uint32_t timeout_ms);` — 0 ok, <0 error/timeout.

- [ ] **Step 1: Write `kernel/lwip_port/ksock_dns.h`**

```c
#ifndef KSOCK_DNS_H
#define KSOCK_DNS_H
#include <stdint.h>
int ksock_dns_query_a(const char* host, uint8_t out_ip[4], uint32_t timeout_ms);
#endif
```

- [ ] **Step 2: Write `kernel/lwip_port/ksock_dns.c`**

```c
#include "ksock_dns.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "include/kernel/net_lwip.h"
#include "include/kernel/task.h"
#include "include/kernel/pit.h"
#include <string.h>

typedef struct { volatile int done; volatile int ok; ip_addr_t addr; } dns_wait_t;

static void dns_cb(const char* name, const ip_addr_t* ipaddr, void* arg) {
    dns_wait_t* w = (dns_wait_t*)arg;
    (void)name;
    if (ipaddr) { w->addr = *ipaddr; w->ok = 1; }
    w->done = 1;
}

int ksock_dns_query_a(const char* host, uint8_t out_ip[4], uint32_t timeout_ms) {
    dns_wait_t w; ip_addr_t resolved; err_t e;
    uint32_t start = (uint32_t)pit_get_ms();
    memset(&w, 0, sizeof(w));
    e = dns_gethostbyname(host, &resolved, dns_cb, &w);
    if (e == ERR_OK) {                 /* cached */
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(&resolved));
        out_ip[0]=(uint8_t)v; out_ip[1]=(uint8_t)(v>>8); out_ip[2]=(uint8_t)(v>>16); out_ip[3]=(uint8_t)(v>>24);
        return 0;
    }
    if (e != ERR_INPROGRESS) return -1;
    while (!w.done) {
        if (timeout_ms && (uint32_t)pit_get_ms() - start >= timeout_ms) return -1;
        net_service_tick();
        task_yield();
    }
    if (!w.ok) return -1;
    {
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(&w.addr));
        out_ip[0]=(uint8_t)v; out_ip[1]=(uint8_t)(v>>8); out_ip[2]=(uint8_t)(v>>16); out_ip[3]=(uint8_t)(v>>24);
    }
    return 0;
}
```

- [ ] **Step 3: Add to Makefile and build**

Add `kernel/lwip_port/ksock_dns.c` to `LWIP_PORT_SRCS`. Run `make 2>&1 | tail -20`. Expected: clean link.

- [ ] **Step 4: Commit**

```bash
git add kernel/lwip_port/ksock_dns.c kernel/lwip_port/ksock_dns.h Makefile
git commit -m "feat(net): DNS resolver wrapper on lwIP dns_gethostbyname"
```

---

## Task 8: TCP socket backend on lwIP raw API

The largest backend: client connect, send, recv, listener/accept, close — all on lwIP raw TCP with per-socket rx buffering.

**Files:**
- Create: `kernel/lwip_port/ksock_tcp.c`
- Create: `kernel/lwip_port/ksock_tcp.h`
- Modify: `Makefile`

**Interfaces:**
- Consumes: lwIP `tcp_new`, `tcp_bind`, `tcp_connect`, `tcp_listen`, `tcp_accept`, `tcp_recv`, `tcp_sent`, `tcp_poll`, `tcp_err`, `tcp_write`, `tcp_output`, `tcp_recved`, `tcp_close`, `tcp_abort`, `tcp_nagle_disable`; `net_service_tick`, `task_yield`, `pit_get_ms`, `kmalloc`/`kfree`.
- Produces:
  - `typedef struct ksock_tcp ksock_tcp_t;`
  - `ksock_tcp_t* ksock_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms);` — NULL on failure.
  - `int ksock_tcp_send(ksock_tcp_t*, const void* buf, uint16_t len, uint32_t timeout_ms, int nonblock);` — bytes, 0 would-block, <0 error.
  - `int ksock_tcp_recv(ksock_tcp_t*, void* buf, uint16_t cap, uint16_t* out_len, uint32_t timeout_ms, int nonblock);` — bytes, 0 = peer-closed, <0 error, special `-EAGAIN`-style 0-with-flag for would-block (use return `-11` for would-block to disambiguate from close).
  - `int ksock_tcp_readable(ksock_tcp_t*); int ksock_tcp_writable(ksock_tcp_t*);`
  - `void ksock_tcp_set_nodelay(ksock_tcp_t*, int on);`
  - `ksock_tcp_t* ksock_tcp_listen(uint16_t port, int backlog);`
  - `ksock_tcp_t* ksock_tcp_accept(ksock_tcp_t* listener, uint32_t timeout_ms, int nonblock);`
  - `int ksock_tcp_peer(ksock_tcp_t*, uint8_t out_ip[4], uint16_t* out_port);`
  - `void ksock_tcp_close(ksock_tcp_t*);`

- [ ] **Step 1: Write `kernel/lwip_port/ksock_tcp.h`**

```c
#ifndef KSOCK_TCP_H
#define KSOCK_TCP_H
#include <stdint.h>

#define KSOCK_TCP_WOULDBLOCK (-11)

typedef struct ksock_tcp ksock_tcp_t;

ksock_tcp_t* ksock_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms);
int  ksock_tcp_send(ksock_tcp_t*, const void* buf, uint16_t len, uint32_t timeout_ms, int nonblock);
int  ksock_tcp_recv(ksock_tcp_t*, void* buf, uint16_t cap, uint16_t* out_len, uint32_t timeout_ms, int nonblock);
int  ksock_tcp_readable(ksock_tcp_t*);
int  ksock_tcp_writable(ksock_tcp_t*);
void ksock_tcp_set_nodelay(ksock_tcp_t*, int on);
ksock_tcp_t* ksock_tcp_listen(uint16_t port, int backlog);
ksock_tcp_t* ksock_tcp_accept(ksock_tcp_t* listener, uint32_t timeout_ms, int nonblock);
int  ksock_tcp_peer(ksock_tcp_t*, uint8_t out_ip[4], uint16_t* out_port);
void ksock_tcp_close(ksock_tcp_t*);

#endif
```

- [ ] **Step 2: Write `kernel/lwip_port/ksock_tcp.c` — struct, callbacks, connect**

```c
#include "ksock_tcp.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "include/kernel/net_lwip.h"
#include "include/kernel/kheap.h"
#include "include/kernel/task.h"
#include "include/kernel/pit.h"
#include <string.h>

#define TCP_RXBUF 8192
#define ACCEPT_Q  4

struct ksock_tcp {
    struct tcp_pcb* pcb;
    uint8_t  rx[TCP_RXBUF];
    uint32_t rx_head, rx_len;     /* ring fill */
    volatile int connected;
    volatile int peer_closed;
    volatile int err;
    /* listener accept queue */
    struct ksock_tcp* acc_q[ACCEPT_Q];
    volatile uint32_t acc_head, acc_tail;
    int is_listener;
};

static err_t on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t e);
static err_t on_sent(void* arg, struct tcp_pcb* pcb, u16_t len);
static void  on_err(void* arg, err_t e);
static err_t on_connected(void* arg, struct tcp_pcb* pcb, err_t e);

static ksock_tcp_t* alloc_sock(struct tcp_pcb* pcb) {
    ksock_tcp_t* s = (ksock_tcp_t*)kmalloc(sizeof(*s));
    if (!s) return 0;
    memset(s, 0, sizeof(*s));
    s->pcb = pcb;
    if (pcb) {
        tcp_arg(pcb, s);
        tcp_recv(pcb, on_recv);
        tcp_sent(pcb, on_sent);
        tcp_err(pcb, on_err);
    }
    return s;
}

static err_t on_connected(void* arg, struct tcp_pcb* pcb, err_t e) {
    ksock_tcp_t* s = (ksock_tcp_t*)arg; (void)pcb;
    if (e == ERR_OK) s->connected = 1; else s->err = 1;
    return ERR_OK;
}

static err_t on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t e) {
    ksock_tcp_t* s = (ksock_tcp_t*)arg;
    if (e != ERR_OK) { if (p) pbuf_free(p); s->err = 1; return ERR_OK; }
    if (!p) { s->peer_closed = 1; return ERR_OK; }   /* FIN */
    {
        uint16_t avail = (uint16_t)(TCP_RXBUF - s->rx_len);
        uint16_t take = p->tot_len < avail ? p->tot_len : avail;
        uint16_t off = 0, wpos = (uint16_t)((s->rx_head + s->rx_len) % TCP_RXBUF);
        while (off < take) {
            uint16_t chunk = (uint16_t)(TCP_RXBUF - wpos);
            if (chunk > (uint16_t)(take - off)) chunk = (uint16_t)(take - off);
            pbuf_copy_partial(p, s->rx + wpos, chunk, off);
            wpos = (uint16_t)((wpos + chunk) % TCP_RXBUF);
            off += chunk;
        }
        s->rx_len += take;
        tcp_recved(pcb, take);
        pbuf_free(p);
    }
    return ERR_OK;
}

static err_t on_sent(void* arg, struct tcp_pcb* pcb, u16_t len) {
    (void)arg; (void)pcb; (void)len; return ERR_OK;   /* writable polled via tcp_sndbuf */
}

static void on_err(void* arg, err_t e) {
    ksock_tcp_t* s = (ksock_tcp_t*)arg; (void)e;
    if (s) { s->err = 1; s->pcb = 0; }   /* pcb already freed by lwIP */
}

ksock_tcp_t* ksock_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms) {
    ip_addr_t dst; struct tcp_pcb* pcb; ksock_tcp_t* s;
    uint32_t start = (uint32_t)pit_get_ms();
    IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);
    pcb = tcp_new();
    if (!pcb) return 0;
    s = alloc_sock(pcb);
    if (!s) { tcp_abort(pcb); return 0; }
    if (tcp_connect(pcb, &dst, port, on_connected) != ERR_OK) { ksock_tcp_close(s); return 0; }
    while (!s->connected && !s->err) {
        if (timeout_ms && (uint32_t)pit_get_ms() - start >= timeout_ms) { ksock_tcp_close(s); return 0; }
        net_service_tick();
        task_yield();
    }
    if (s->err) { ksock_tcp_close(s); return 0; }
    return s;
}
```

- [ ] **Step 3: Add send/recv/readable/writable/nodelay to `ksock_tcp.c`**

```c
int ksock_tcp_readable(ksock_tcp_t* s) { return s && (s->rx_len > 0 || s->peer_closed); }
int ksock_tcp_writable(ksock_tcp_t* s) { return s && s->pcb && tcp_sndbuf(s->pcb) > 0; }

void ksock_tcp_set_nodelay(ksock_tcp_t* s, int on) {
    if (!s || !s->pcb) return;
    if (on) tcp_nagle_disable(s->pcb); else tcp_nagle_enable(s->pcb);
}

int ksock_tcp_send(ksock_tcp_t* s, const void* buf, uint16_t len, uint32_t timeout_ms, int nonblock) {
    uint32_t start = (uint32_t)pit_get_ms();
    uint16_t sent = 0;
    const uint8_t* p = (const uint8_t*)buf;
    if (!s || !s->pcb) return -1;
    while (sent < len) {
        if (s->err) return -1;
        u16_t space = tcp_sndbuf(s->pcb);
        if (space == 0) {
            if (nonblock) return sent > 0 ? (int)sent : KSOCK_TCP_WOULDBLOCK;
            if (timeout_ms && (uint32_t)pit_get_ms() - start >= timeout_ms) return sent > 0 ? (int)sent : 0;
            net_service_tick(); task_yield(); continue;
        }
        {
            u16_t chunk = (uint16_t)((len - sent) < space ? (len - sent) : space);
            err_t e = tcp_write(s->pcb, p + sent, chunk, TCP_WRITE_FLAG_COPY);
            if (e == ERR_MEM) { net_service_tick(); task_yield(); continue; }
            if (e != ERR_OK) return -1;
            sent += chunk;
            tcp_output(s->pcb);
        }
    }
    return (int)sent;
}

int ksock_tcp_recv(ksock_tcp_t* s, void* buf, uint16_t cap, uint16_t* out_len,
                   uint32_t timeout_ms, int nonblock) {
    uint32_t start = (uint32_t)pit_get_ms();
    if (!s) return -1;
    for (;;) {
        if (s->rx_len > 0) {
            uint16_t n = s->rx_len < cap ? (uint16_t)s->rx_len : cap;
            uint16_t off = 0;
            while (off < n) {
                uint16_t chunk = (uint16_t)(TCP_RXBUF - s->rx_head);
                if (chunk > (uint16_t)(n - off)) chunk = (uint16_t)(n - off);
                memcpy((uint8_t*)buf + off, s->rx + s->rx_head, chunk);
                s->rx_head = (uint16_t)((s->rx_head + chunk) % TCP_RXBUF);
                off += chunk;
            }
            s->rx_len -= n;
            if (out_len) *out_len = n;
            return (int)n;
        }
        if (s->peer_closed) { if (out_len) *out_len = 0; return 0; }
        if (s->err) return -1;
        if (nonblock) return KSOCK_TCP_WOULDBLOCK;
        if (timeout_ms && (uint32_t)pit_get_ms() - start >= timeout_ms) { if (out_len) *out_len = 0; return KSOCK_TCP_WOULDBLOCK; }
        net_service_tick(); task_yield();
    }
}
```

- [ ] **Step 4: Add listener/accept/peer/close to `ksock_tcp.c`**

```c
static err_t on_accept(void* arg, struct tcp_pcb* newpcb, err_t e) {
    ksock_tcp_t* lst = (ksock_tcp_t*)arg;
    uint32_t next;
    if (e != ERR_OK || !newpcb) return ERR_VAL;
    next = (lst->acc_head + 1u) % ACCEPT_Q;
    if (next == lst->acc_tail) return ERR_ABRT;     /* backlog full */
    {
        ksock_tcp_t* s = alloc_sock(newpcb);
        if (!s) return ERR_ABRT;
        s->connected = 1;
        lst->acc_q[lst->acc_head] = s;
        lst->acc_head = next;
    }
    return ERR_OK;
}

ksock_tcp_t* ksock_tcp_listen(uint16_t port, int backlog) {
    struct tcp_pcb* pcb = tcp_new();
    ksock_tcp_t* s;
    if (!pcb) return 0;
    if (tcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) { tcp_abort(pcb); return 0; }
    pcb = tcp_listen_with_backlog(pcb, (u8_t)(backlog > 0 ? backlog : 1));
    if (!pcb) return 0;
    s = (ksock_tcp_t*)kmalloc(sizeof(*s));
    if (!s) { tcp_abort(pcb); return 0; }
    memset(s, 0, sizeof(*s));
    s->pcb = pcb; s->is_listener = 1;
    tcp_arg(pcb, s);
    tcp_accept(pcb, on_accept);
    return s;
}

ksock_tcp_t* ksock_tcp_accept(ksock_tcp_t* lst, uint32_t timeout_ms, int nonblock) {
    uint32_t start = (uint32_t)pit_get_ms();
    if (!lst || !lst->is_listener) return 0;
    for (;;) {
        if (lst->acc_tail != lst->acc_head) {
            ksock_tcp_t* s = lst->acc_q[lst->acc_tail];
            lst->acc_tail = (lst->acc_tail + 1u) % ACCEPT_Q;
            return s;
        }
        if (nonblock) return 0;
        if (timeout_ms && (uint32_t)pit_get_ms() - start >= timeout_ms) return 0;
        net_service_tick(); task_yield();
    }
}

int ksock_tcp_peer(ksock_tcp_t* s, uint8_t out_ip[4], uint16_t* out_port) {
    if (!s || !s->pcb) return -1;
    {
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(&s->pcb->remote_ip));
        out_ip[0]=(uint8_t)v; out_ip[1]=(uint8_t)(v>>8); out_ip[2]=(uint8_t)(v>>16); out_ip[3]=(uint8_t)(v>>24);
        if (out_port) *out_port = s->pcb->remote_port;
    }
    return 0;
}

void ksock_tcp_close(ksock_tcp_t* s) {
    if (!s) return;
    if (s->pcb) {
        tcp_arg(s->pcb, NULL);
        tcp_recv(s->pcb, NULL); tcp_sent(s->pcb, NULL); tcp_err(s->pcb, NULL);
        if (s->is_listener) { tcp_close(s->pcb); }
        else if (tcp_close(s->pcb) != ERR_OK) { tcp_abort(s->pcb); }
    }
    kfree(s);
}
```

- [ ] **Step 5: Add to Makefile and build**

Add `kernel/lwip_port/ksock_tcp.c` to `LWIP_PORT_SRCS`. Run `make 2>&1 | tail -20`. Expected: clean link.

- [ ] **Step 6: Commit**

```bash
git add kernel/lwip_port/ksock_tcp.c kernel/lwip_port/ksock_tcp.h Makefile
git commit -m "feat(net): TCP socket backend on lwIP raw API"
```

---

## Task 9: Rewire `syscall.c` socket nodes onto the ksock backends

Re-point the `ksock_node_impl_t` VFS node ops at the new `ksock_udp`/`ksock_tcp` backends. Keep the userland ABI **unchanged in this task** (per-call timeouts still accepted) so the change is isolated; the ABI cleanup happens in Task 11.

**Files:**
- Modify: `kernel/syscall.c` (the `ksock_*` ops, `sys_socket`, send/recv/sendto/recvfrom/connect/accept/listen/bind handlers, setsockopt/getsockopt, `sys_dns_query`)

**Interfaces:**
- Consumes: all `ksock_udp_*`, `ksock_tcp_*`, `ksock_dns_query_a` from Tasks 6–8.
- Produces: socket syscalls backed by lwIP; `net_*` (old stack) no longer referenced from `syscall.c`.

- [ ] **Step 1: Read the current socket syscall handlers**

Run:
```bash
sed -n '380,640p' kernel/syscall.c
```
Expected: full view of `sys_poll` socket cases, `setsockopt`/`getsockopt`, `sys_socket`, send/recv/sendto/recvfrom/connect/accept/listen/bind, and `sys_dns_query` (line ~483). Note every `net_*` call to replace.

- [ ] **Step 2: Change `ksock_node_impl_t` to hold backend pointers**

In `kernel/syscall.c`, replace the `int32_t id;` field with:
```c
    void* udp;     /* ksock_udp_t*  when role == DGRAM */
    void* tcp;     /* ksock_tcp_t*  when role == STREAM_CONN or STREAM_LISTENER */
```
Add includes:
```c
#include "lwip_port/ksock_udp.h"
#include "lwip_port/ksock_tcp.h"
#include "lwip_port/ksock_dns.h"
```
(Confirm the include path resolves; `-Ikernel/lwip_port` is on `CPPFLAGS` from Task 3, so `#include "ksock_udp.h"` also works — use whichever matches the other includes' style.)

- [ ] **Step 3: Re-point `ksock_read`/`ksock_write`/`ksock_close`**

Replace the bodies that called `net_tcp_client_recv`/`net_tcp_client_send`/`net_*_close` with calls to `ksock_tcp_recv`/`ksock_tcp_send`/`ksock_tcp_close`/`ksock_udp_close`, mapping the role. Example `ksock_read`:
```c
static int32_t ksock_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    ksock_node_impl_t* impl; uint16_t out_len = 0; int rc; uint32_t to; (void)offset;
    if (!node || !buffer) return -1;
    impl = (ksock_node_impl_t*)node->impl;
    if (!impl || impl->magic != KSOCK_NODE_MAGIC) return -1;
    if (impl->shut_rd) return 0;
    if (impl->role != KSOCK_ROLE_STREAM_CONN || !impl->tcp) return -1;
    to = impl->nonblock ? 0u : ksock_effective_timeout(impl->rcv_timeout_ms, 0u);
    rc = ksock_tcp_recv((ksock_tcp_t*)impl->tcp, buffer, (uint16_t)size, &out_len, to, impl->nonblock);
    if (rc >= 0) return (int32_t)out_len;        /* 0 => peer closed */
    if (rc == KSOCK_TCP_WOULDBLOCK) return impl->nonblock ? -1 /*EAGAIN*/ : 0;
    return -1;
}
```
Apply the analogous change to `ksock_write` (→ `ksock_tcp_send`) and `ksock_close` (→ `ksock_tcp_close`/`ksock_udp_close` by role).

- [ ] **Step 4: Re-point `sys_socket` and the connect/bind/listen/accept/sendto/recvfrom handlers**

For each handler currently calling `net_udp_socket_open`/`net_tcp_client_connect`/`net_tcp_listener_*`/`net_udp_socket_sendto`/`net_udp_socket_recvfrom`, substitute the `ksock_*` equivalent and store the returned pointer in `impl->udp`/`impl->tcp`. Connect example:
```c
/* inside sys_connect handler, TCP path */
ksock_tcp_t* t = ksock_tcp_connect(dst_ip, dst_port, timeout_ms);
if (!t) return -1;
impl->tcp = t;
impl->role = KSOCK_ROLE_STREAM_CONN;
return 0;
```
Accept example (listener → new conn node):
```c
ksock_tcp_t* c = ksock_tcp_accept((ksock_tcp_t*)impl->tcp, timeout_ms, impl->nonblock);
if (!c) return -1;  /* timeout/would-block */
new_node = ksock_make_node(KSOCK_ROLE_STREAM_CONN, 0, KSOCK_DEFAULT_TIMEOUT_MS);
((ksock_node_impl_t*)new_node->impl)->tcp = c;
```
Re-point the `sys_poll` readability/writability checks:
```c
if ((events & KPOLLIN)  && ksock_udp_readable((ksock_udp_t*)impl->udp)) revents |= KPOLLIN;       /* DGRAM */
if ((events & KPOLLIN)  && ksock_tcp_readable((ksock_tcp_t*)impl->tcp)) revents |= KPOLLIN;        /* CONN */
if ((events & KPOLLOUT) && ksock_tcp_writable((ksock_tcp_t*)impl->tcp)) revents |= KPOLLOUT;
```
Re-point `setsockopt` `TCP_NODELAY` to `ksock_tcp_set_nodelay`. For `SO_RCVTIMEO`/`SO_SNDTIMEO`/`SO_REUSEADDR`/keepalive, keep storing in the impl flags as today (lwIP keepalive can be left as a stored flag for now).

- [ ] **Step 5: Re-point `sys_dns_query` (line ~483)**

Replace:
```c
return (long)net_dns_query_a(args->hostname, args->dns_server_ip, ...);
```
with:
```c
uint8_t ip[4];
if (ksock_dns_query_a(args->hostname, ip, /*timeout*/ 5000u) != 0) return -1;
/* copy ip into the args' out field exactly as the old code did */
```
(Match the exact out-parameter mechanism the old `net_dns_query_a` used — inspect `args` struct fields in Step 1.)

- [ ] **Step 6: Build**

Run `make 2>&1 | tail -30`. Expected: clean link. `syscall.c` no longer references any `net_*` symbol (verify: `grep -nE "net_(tcp|udp|dns|ping|arp)" kernel/syscall.c` → no matches).

- [ ] **Step 7: Commit**

```bash
git add kernel/syscall.c
git commit -m "refactor(net): back socket syscalls with lwIP ksock backends"
```

---

## Task 10: Rework the `net` shell command onto lwIP

`kernel/shell.c`'s `cmd_net` is heavily tied to old stack stats. Reduce it to what lwIP can answer, keep the user-facing subcommands that still make sense (`ip`, `ping`, `dhcp`, `dns`, `tcp http`), and drop/replace the deep debug subcommands (`netstat`, `rxdefer`, `timers`, `p2`, `arp`, `test regress|fuzz|stress`) that depended on `net_*` internals.

**Files:**
- Modify: `kernel/shell.c` (`cmd_net` and its help string; the embedded DHCP-client block at lines ~2936–3118 is deleted since lwIP runs DHCP)

**Interfaces:**
- Consumes: `net_lwip_get_ipv4`/`_netmask`/`_gateway`/`_mac`, `net_lwip_driver_name`, `net_lwip_is_ready`, `ksock_dns_query_a`, `ksock_tcp_connect`/`send`/`recv`/`close`. For `ping`, use lwIP: see Step 2.
- Produces: a slimmer `net` command; no references to `net_get_stats`/`net_get_arp_*`/`net_tcp_get_debug_stats`/`net_udp_socket_*`/`net_dhcp_*`.

- [ ] **Step 1: Inventory every old symbol used by the shell**

Run:
```bash
grep -nE "net_[a-z_]+\(" kernel/shell.c | sort -u
```
Expected: the full list to remove/replace. Anything not covered by `net_lwip_*`/`ksock_*` must have its subcommand removed.

- [ ] **Step 2: Replace `net ip`/default print and delete the manual DHCP block**

- `net` / `net ip`: print MAC, IP, netmask, gateway, driver name, link state using `net_lwip_*` getters. Remove the `net_get_stats`/`arp`/`tcp debug` printouts (or replace with lwIP `stats_display()` only if `LWIP_STATS` is enabled — otherwise drop).
- Delete the entire embedded DHCP discover/request loop (~lines 2936–3118): lwIP's `dhcp_start` already ran at boot. Replace the `net dhcp` subcommand body with a status print of the current lease (the `net_lwip_get_ipv4*` values) and, optionally, a `dhcp_renew(deanos_netif_default())` trigger (include `lwip/dhcp.h` and `deanos_netif.h`).

- [ ] **Step 3: Replace `net ping <ip>` using lwIP**

lwIP has no synchronous ping API in core; use the contrib ping app pattern inline with a raw PCB, OR (simpler, recommended) keep ping minimal by issuing it through the existing ICMP via a tiny raw-pcb helper added to `lwip_glue.c`:
```c
/* in lwip_glue.c — minimal blocking ping using RAW ICMP echo */
int net_lwip_ping(const uint8_t ip[4], uint16_t seq, uint32_t timeout_ms);
```
Implement `net_lwip_ping` with `raw_new(IP_PROTO_ICMP)`, build an ICMP echo request, `raw_sendto`, set a `raw_recv` callback that matches the echo id/seq and flips a done flag, then block via `net_service_tick()`+`task_yield()` until reply or timeout. Declare it in `net_lwip.h`. Call it from `cmd_net`'s ping path.

> This is the authoritative end-to-end ping-over-lwIP check deferred from Task 5.

- [ ] **Step 4: Replace `net dns <host>` and `net tcp http <host> <port> <path>`**

- `net dns <host>`: call `ksock_dns_query_a(host, ip, 5000)` and print the result.
- `net tcp http ...`: `ksock_dns_query_a` to resolve host → `ksock_tcp_connect` → `ksock_tcp_send` the `GET` request → loop `ksock_tcp_recv` printing until 0 (closed) → `ksock_tcp_close`.

- [ ] **Step 5: Remove dead subcommands and fix the help string**

Delete `netstat`, `rxdefer`, `timers`, `p2`, `arp`, `arping`, `tx`, `regs`, `test regress|fuzz|stress` branches (all depend on removed `net_*` internals). Update the `{"net", ...}` help string at `kernel/shell.c:158` to list only the surviving subcommands.

- [ ] **Step 6: Build and run**

Run:
```bash
make run-net
```
In the shell, verify:
```
net                       # shows DHCP IP in 10.0.2.x, gw 10.0.2.2, driver e1000, link up
net ping 10.0.2.2         # ICMP reply via lwIP
net dns example.com       # resolves to an A record
net tcp http example.com 80 /   # prints an HTTP/1.x response
```
Expected: all four behave; no crash; no hang (timeouts return).

- [ ] **Step 7: Commit**

```bash
git add kernel/shell.c kernel/lwip_port/lwip_glue.c kernel/include/kernel/net_lwip.h
git commit -m "refactor(net): rework net shell command onto lwIP (ip/ping/dns/http/dhcp)"
```

---

## Task 11: Userland ABI cleanup — standard fd-based BSD sockets

Now that the kernel is fully on lwIP, simplify the syscall ABI and libc to standard signatures. Drop per-call timeouts (move to `SO_RCVTIMEO`/`SO_SNDTIMEO` + `O_NONBLOCK`), drop `closesocket` (alias only), standardize `connect`/`accept`/`send`/`recv`/`sendto`/`recvfrom`.

**Files:**
- Modify: `libc/include/sys/socket.h`
- Modify: `libc/unistd/syscalls.c`
- Modify: `kernel/syscall.c` (arg structs + handler signatures)
- Modify: `kernel/include/kernel/syscall.h` (if arg structs live there — confirm)
- Modify: `libc/netdb/resolve.c` if it used the old `connect` timeout arg

**Interfaces:**
- Consumes: the lwIP-backed handlers from Task 9.
- Produces: BSD-standard libc signatures; sockets usable with `read`/`write`/`close`/`poll`/`fcntl`.

- [ ] **Step 1: Confirm where the packed arg structs are defined**

Run:
```bash
grep -rnE "syscall_(sendto|recvfrom|connect|accept|send|recv)_args_t" kernel/include kernel/syscall.c libc
```
Expected: locate each struct; these get their `timeout_ms` field removed (replace with a `flags` field where appropriate).

- [ ] **Step 2: Update `libc/include/sys/socket.h` to standard signatures**

```c
int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr_in* addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr_in* addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen);
ssize_t send(int sockfd, const void* buf, size_t len, int flags);
ssize_t recv(int sockfd, void* buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void* buf, size_t len, int flags, const struct sockaddr_in* dest, socklen_t addrlen);
ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr_in* src, socklen_t* addrlen);
int shutdown(int sockfd, int how);
int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen);
int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen);

#define MSG_DONTWAIT 0x40

/* Deprecated alias kept for one transition; prefer close(). */
int closesocket(int sockfd);
```
Add the `flags` (`MSG_DONTWAIT`) handling and drop `connect`'s `timeout_ms`/`accept`'s `timeout_ms`. Keep `O_NONBLOCK` semantics via `fcntl`.

- [ ] **Step 3: Update `libc/unistd/syscalls.c`**

Rewrite each wrapper to the new signatures, filling the (now timeout-free) arg structs. `closesocket` becomes:
```c
int closesocket(int sockfd) { return close(sockfd); }
```
Add `MSG_DONTWAIT` → set the per-call nonblock bit passed to the kernel handler.

- [ ] **Step 4: Update kernel handlers in `syscall.c`**

Remove `timeout_ms` from the arg structs and handlers. In send/recv/sendto/recvfrom, derive blocking from `(flags & MSG_DONTWAIT) || impl->nonblock`; derive timeout from `impl->rcv_timeout_ms`/`snd_timeout_ms` (set by `SO_RCVTIMEO`/`SO_SNDTIMEO`). `connect`/`accept` use `impl->snd_timeout_ms`/a default, not a per-call arg.

- [ ] **Step 5: Add `fcntl` `O_NONBLOCK` support for socket fds**

Confirm `fcntl` exists:
```bash
grep -rnE "fcntl|F_SETFL|O_NONBLOCK" kernel/syscall.c libc/include libc/unistd
```
If `F_SETFL`/`F_GETFL` aren't handled for socket nodes, add them: `F_SETFL` sets `impl->nonblock` from `O_NONBLOCK`; `F_GETFL` returns it. If `fcntl` doesn't exist at all, add a minimal one limited to these flags (note in commit message). 

- [ ] **Step 6: Build the whole image (kernel + libc + user)**

Run:
```bash
make clean && make 2>&1 | tail -30
```
Expected: clean build. Fix any user `.s` programs only if they call sockets (they are asm test stubs — likely unaffected).

- [ ] **Step 7: Update any in-kernel callers of the old libc-style socket signatures**

Run:
```bash
grep -rnE "connect\(|accept\(|sendto\(|recvfrom\(|closesocket\(" kernel/shell.c
```
Update `cmd_net`'s `tcp http` path if it used libc socket wrappers (it uses `ksock_*` directly per Task 10, so likely no change). Rebuild.

- [ ] **Step 8: Run and smoke-test sockets end to end**

Run `make run-net`, then exercise `net tcp http example.com 80 /` and `net dns example.com` again. Expected: still work with the new ABI.

- [ ] **Step 9: Commit**

```bash
git add libc/include/sys/socket.h libc/unistd/syscalls.c kernel/syscall.c kernel/include/kernel/syscall.h libc/netdb/resolve.c
git commit -m "refactor(net): standardize socket ABI to fd-based BSD sockets"
```

---

## Task 12: CI — checkout submodule before build

**Files:**
- Modify: `.github/workflows/*.yml`

**Interfaces:**
- Produces: CI that initializes the lwIP submodule so the ISO build succeeds.

- [ ] **Step 1: Inspect the workflow**

Run:
```bash
ls .github/workflows && cat .github/workflows/*.yml
```
Expected: find the `actions/checkout` step and the build step.

- [ ] **Step 2: Enable submodule checkout**

In the `actions/checkout@vN` step, add:
```yaml
        with:
          submodules: recursive
```
If checkout is done manually via `git`, add before the build:
```yaml
      - run: git submodule update --init --recursive
```

- [ ] **Step 3: Commit**

```bash
git add .github/workflows
git commit -m "build(net): init lwIP submodule in CI before building ISO"
```

---

## Task 13: Verification pass (ping / DNS / TCP / DHCP) on both NICs

**Files:** none (verification only)

- [ ] **Step 1: e1000 smoke test**

Run `make run-net`. In shell run, in order:
```
net
net ping 10.0.2.2
net dns example.com
net tcp http example.com 80 /
```
Expected: DHCP IP `10.0.2.x`/gw `10.0.2.2`; ping reply; DNS A record; HTTP response text. Record pass/fail of each.

- [ ] **Step 2: rtl8139 smoke test**

Run `make run-net-rtl`. Repeat the same four commands. Expected: same behavior via the rtl8139 driver path.

- [ ] **Step 3: No-NIC boot**

Run `make run` (no `-device`). Expected: boots cleanly; `net` reports no NIC / link down; no crash; no hang in `net_service_tick` (it returns early when `!g_started`).

- [ ] **Step 4: Commit a short verification note**

Append results to the design or a `NETWORKING.md` note and:
```bash
git add -A && git commit -m "test(net): record lwIP bring-up verification on e1000 and rtl8139"
```

---

## Task 14: Delete the old stack

Only after Task 13 passes on both NICs.

**Files:**
- Delete: `kernel/net.c`, `kernel/dns.c`, `kernel/dhcp.c`
- Delete (if now unused): `kernel/include/kernel/net.h`, `kernel/include/kernel/net_dns.h`, `kernel/include/kernel/net_dhcp.h`
- Modify: `Makefile` (remove the three sources), any leftover includes

- [ ] **Step 1: Confirm nothing references the old stack**

Run:
```bash
grep -rnE "net_(core_init|initialize|poll|tcp_client|udp_socket|dns_query_a|ping_ipv4|get_arp|get_stats|dhcp_)" kernel libc | grep -v "kernel/net.c\|kernel/dns.c\|kernel/dhcp.c"
```
Expected: **no matches**. If any remain, fix the caller first (it should use `net_lwip_*`/`ksock_*`).

- [ ] **Step 2: Remove sources from the Makefile**

In `Makefile` `KERNEL_SRCS`, delete the three lines:
```
kernel/net.c \
kernel/dhcp.c \
kernel/dns.c \
```

- [ ] **Step 3: Delete the files**

```bash
git rm kernel/net.c kernel/dns.c kernel/dhcp.c
```
Then check the headers:
```bash
grep -rn "kernel/net.h\|net_dns.h\|net_dhcp.h" kernel libc
```
`git rm` each header that has no remaining includers. Fix any `#include` that still points at a removed header (e.g. `shell.c` included `net_dhcp.h` for `net_dhcp_parse_options` — that block was deleted in Task 10; remove the include).

- [ ] **Step 4: Build clean and run**

Run:
```bash
make clean && make 2>&1 | tail -20 && make run-net
```
Expected: builds with `net.c`/`dns.c`/`dhcp.c` gone; the four smoke commands still pass.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor(net): remove hand-rolled network stack (replaced by lwIP)"
```

---

## Self-Review notes (addressed)

- **Spec coverage:** port layer (Tasks 2–4) ✓; static heap (Task 2 lwipopts) ✓; deferred RX into `netif->input` (Task 4) ✓; single `net_service_tick` pump (Task 5) ✓; DHCP via lwIP + delete `dhcp.c` (Tasks 5, 14) ✓; DNS via lwIP + delete `dns.c` (Tasks 7, 14) ✓; raw API / no lwIP socket layer (Task 2 `LWIP_SOCKET=0`; Tasks 6–8) ✓; socket syscalls rewired (Task 9) ✓; BSD ABI cleanup, drop per-call timeouts, `closesocket` alias, `fcntl`/`O_NONBLOCK` (Task 11) ✓; submodule pinned STABLE-2_2_0 + `Filelists.mk` (Tasks 1, 3) ✓; CI submodule init (Task 12) ✓; incremental ping→DNS→TCP→ABI→delete order ✓; keep NIC drivers (Task 4 reuses `*_send_raw`/`*_set_rx_callback`) ✓.
- **Confirm-then-use:** several steps begin with a `grep` to confirm exact symbol names (`pit_get_ms`, `task_yield`, driver `is_ready`/`get_mac`, arg-struct locations) before writing code, because those names were not all verified during planning. Use the confirmed names, not the placeholders.
- **Type consistency:** `ksock_udp_t`/`ksock_tcp_t` opaque types and their function signatures are defined once (Tasks 6, 8) and consumed unchanged in Task 9; `KSOCK_TCP_WOULDBLOCK` defined in `ksock_tcp.h` and used in both backend and syscall layer.
