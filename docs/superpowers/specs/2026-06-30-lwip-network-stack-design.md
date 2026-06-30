# Replace custom network stack with lwIP

**Date:** 2026-06-30
**Status:** Approved (brainstorm) — ready for implementation planning

## Motivation

The current hand-rolled stack (`kernel/net.c`, ~3970 lines, doing
ARP/IPv4/ICMP/UDP/TCP plus `dns.c`/`dhcp.c`) has been rewritten multiple times
and TCP in particular is unreliable. Replacing it with lwIP serves three goals
at once: get a battle-tested, correct stack; gain features that are tedious to
hand-write; and learn how a real third-party stack is ported into a hobby OS.

## Decisions (locked during brainstorming)

- **Integration mode:** `NO_SYS=1` (raw/callback API, single-threaded). No
  `sys_arch` threading layer. Migration to `NO_SYS=0` is possible later but out
  of scope here.
- **Vendoring:** lwIP as a **git submodule** pinned to release tag
  **STABLE-2_2_0**.
- **DHCP/DNS:** Use lwIP's built-in `dhcp` and `dns` modules; delete the custom
  `dhcp.c`/`dns.c`.
- **Userland ABI:** Clean up toward **standard fd-based BSD sockets** (option 2),
  not a byte-for-byte preservation of the current custom ABI.
- **lwIP heap:** **Static lwIP heap** (`MEM_LIBC_MALLOC=0`, fixed `MEM_SIZE`),
  isolating network buffer churn from the kernel heap.

## Architecture

### Delete
- `kernel/net.c` — entire hand-rolled protocol stack, rx defer queue, netif
  abstraction.
- `kernel/dns.c`, `kernel/dhcp.c` — replaced by lwIP's `dns` / `dhcp`.

### Keep (hardware logic unchanged)
- `kernel/e1000.c`, `kernel/rtl8139.c`. Only their existing hooks are
  re-targeted at the lwIP netif glue:
  - TX: `e1000_send_raw(data,len)` / `rtl8139_send_raw(data,len)`
  - RX: `*_set_rx_callback(cb)` where `cb(const uint8_t* frame, uint16_t len)`
  - `*_get_mac(out[6])`, `*_link_up()`, `*_is_ready()`

### Add — `kernel/lwip_port/`
The `NO_SYS=1` port layer:

- **`lwipopts.h`**
  - `NO_SYS=1`, `SYS_LIGHTWEIGHT_PROT=0` (protect RX with IRQ disable).
  - `LWIP_NETCONN=0`, `LWIP_SOCKET=0` — raw API only; the BSD socket layer is
    implemented by the kernel syscall code, not lwIP.
  - Enable `LWIP_ARP`, `LWIP_IPV4`, `LWIP_ICMP`, `LWIP_UDP`, `LWIP_TCP`,
    `LWIP_DHCP`, `LWIP_DNS`. IPv6 off initially (flip on later).
  - `MEM_LIBC_MALLOC=0`, static heap. Start conservative (e.g. `MEM_SIZE=64KB`,
    ~a dozen TCP PCBs via `MEMP_NUM_*`); tune from observed stats.
  - `LWIP_STATS` on in debug builds, off in release.
- **`arch/cc.h`** — route `LWIP_PLATFORM_DIAG`/`LWIP_PLATFORM_ASSERT` into the
  kernel log/panic; `PACK_STRUCT_*`; rely on `<stdint.h>` types. i386 is
  little-endian.
- **`sys_now()` / `sys_jiffies()`** — milliseconds from the PIT tick counter.
- **`deanos_netif.c`** — netif glue:
  - `netif->linkoutput` flattens the pbuf chain into a contiguous frame and
    calls the driver `*_send_raw`.
  - `netif->output = etharp_output`; flags
    `NETIF_FLAG_ETHARP | NETIF_FLAG_BROADCAST | NETIF_FLAG_LINK_UP`.
  - hwaddr from `*_get_mac`.
  - **RX path:** driver `rx_callback` runs in IRQ context → copies frame into a
    `PBUF_POOL` pbuf and enqueues it. `net_service_tick()` (non-IRQ context)
    dequeues and calls `netif->input` (`ethernet_input`). The stack never runs
    at interrupt time. (Reuses the spirit of the old rx defer queue.)

### Driving the stack
A single `net_service_tick()`:
1. drains the RX queue into `netif->input`,
2. calls `sys_check_timeouts()`.

Called from the main scheduler/idle loop and after RX IRQs. **All lwIP raw-API
calls happen only from this pump/syscall context — never from IRQ** (raw API is
not reentrant).

### Rewrite — socket syscall layer (`kernel/syscall.c`)
`ksock_node_impl_t` holds a `struct tcp_pcb*` / `struct udp_pcb*` instead of an
`int32_t id`. Per-socket receive buffering bridges the callback API to
read/recv:

- **TCP:** `tcp_recv` appends incoming pbufs to a per-socket rx chain;
  `tcp_sent`/`tcp_poll` track write space; `tcp_err` flags reset/close. `read`
  pulls from the rx chain; `write` calls `tcp_write` + `tcp_output`. `tcp_accept`
  pushes new pcbs onto a listener backlog queue.
- **UDP:** `udp_recv` queues `(src_addr, pbuf)`; `recvfrom` dequeues.
- **Blocking:** blocking `recv`/`accept`/`connect` loop as: check queue/state →
  if empty and blocking, `net_service_tick()` + `task_yield()` → repeat until
  data / timeout / error. `O_NONBLOCK` returns `EWOULDBLOCK` immediately.

Sockets remain fd-backed VFS nodes (already true), so `read`/`write`/`close`/
`poll`/`dup` work on them.

### Trim — `kernel/include/kernel/net.h`
Reduce to a small public API: `net_init`, `net_service_tick`, address/config
reporting, DNS query wrapper. The full stack declarations go away.

## Userland ABI cleanup (option 2)

- Sockets are ordinary fds: `read`/`write`/`close`/`poll`/`dup` all work.
- **Drop `closesocket`** → use `close`. Keep `closesocket` as a deprecated libc
  alias for one transition, then remove.
- **`connect`** loses the bespoke `timeout_ms` arg → `connect(fd, addr, addrlen)`;
  blocking vs nonblocking governed by `O_NONBLOCK`.
- **`send`/`recv`/`sendto`/`recvfrom`** lose per-call `timeout_ms` → standard
  signatures with a `flags` arg (`MSG_DONTWAIT` honored; others ignored).
  Timeouts move to `SO_RCVTIMEO`/`SO_SNDTIMEO` via `setsockopt`.
- **`accept`** loses `timeout_ms` → `accept(fd, addr, addrlen)`.
- Add `fcntl(F_GETFL/F_SETFL, O_NONBLOCK)` for sockets.
- Simplify the packed arg-structs (`syscall_sendto_args_t`, etc.) to match.
- Update shell commands (ping, netstat-lite, dhcp) to the new signatures.

Affected files: `kernel/syscall.c`, `libc/unistd/syscalls.c`,
`libc/include/sys/socket.h`, `kernel/shell.c`.

## DHCP & DNS

- **DHCP:** `dhcp_start(netif)` on link-up; lwIP handles lease/renew/rebind via
  `sys_check_timeouts()`. `net_init` order: bring up driver → set hwaddr →
  `netif_add` → `netif_set_up` → `dhcp_start`. The `dhcp` shell command reports
  `netif->ip_addr`/gw/mask + lease state and can trigger `dhcp_renew()`.
- **DNS:** lwIP `dns_setserver` + `dns_gethostbyname`. `net_dns_query_a` syscall
  becomes a wrapper that kicks the query then blocks via the
  `net_service_tick()`+`yield` loop until the callback fires or it times out.
  DHCP-supplied DNS servers register automatically. libc `resolve.c` unchanged.

## Build integration

- Submodule `third_party/lwip/` pinned to **STABLE-2_2_0**.
- `Makefile`: include lwIP's `Filelists.mk` for the source list (core, ipv4,
  netif; not the api/ socket layer); add
  `-I third_party/lwip/src/include -I kernel/lwip_port` to `CFLAGS`; compile into
  the same freestanding `-m32` objects as the kernel. Objects under
  `build/lwip/`.
- CI workflow: `git submodule update --init --recursive` before the ISO build.

## Migration & testing (incremental; kernel always builds)

1. Vendor + build lwIP with a **stub netif** (no driver) — proves the port layer
   compiles/links freestanding.
2. Wire **one driver** (e1000 under QEMU `-netdev user`) → netif up → DHCP lease
   → **ICMP ping** out. Validates RX/TX glue end-to-end.
3. **UDP** path → DNS resolves a name.
4. **TCP** path → rewire socket syscalls → TCP client connect/send/recv works.
5. **ABI cleanup** → update libc + shell to new signatures; remove `closesocket`
   internals.
6. **Delete** `net.c`/`dns.c`/`dhcp.c` once parity is reached.

Optionally keep the old stack compilable behind a build flag until step 6 as a
fallback, then remove.

**Verification:** QEMU smoke tests (`ping`, DNS lookup, TCP echo/HTTP-GET) plus
the CI ISO build.

## Out of scope (YAGNI for now)

- IPv6.
- `NO_SYS=0` / lwIP socket+netconn layer.
- TLS / application protocols.
- SMP-safe locking of the stack (single-threaded pump assumed).
