# hyper-linux

Run static and dynamically-linked aarch64-linux ELF binaries on macOS Apple Silicon via Hypervisor.framework.

## Architecture

- **hl.c** — Main entry point. CLI (incl. --fork-child, --sysroot), VM setup, interpreter loading (~500 lines).
- **guest.c/h** — Guest memory management. Page tables, read/write, brk/mmap, reset, region tracking.
- **elf.c/h** — ELF64 parser/loader. PT_LOAD segments, PT_INTERP parsing, ET_DYN load_base support.
- **syscall.c/h** — Core infrastructure (FD table, errno/flag translation, brk/mmap) + dispatch switch (~900 lines).
- **syscall_internal.h** — Shared declarations for syscall module helpers.
- **syscall_fs.c/h** — Filesystem: stat, open, close, directory, xattr, permissions (~960 lines).
- **syscall_io.c/h** — I/O: read/write, ioctl, splice, sendfile, poll/select (~610 lines).
- **syscall_inotify.c/h** — inotify emulation via kqueue EVFILT_VNODE (~350 lines).
- **syscall_time.c/h** — Time: clock_gettime, nanosleep, gettimeofday, setitimer (~190 lines).
- **syscall_sys.c/h** — System info: uname, getrandom, sysinfo, prlimit64 (~240 lines).
- **syscall_signal.c/h** — Signal delivery: rt_sigframe, rt_sigaction, delivery, ITIMER_REAL (~520 lines).
- **syscall_net.c/h** — Socket networking: AF/sockaddr/sockopt translation (~670 lines).
- **syscall_proc.c/h** — Process state, accessors, wait4/waitid, vCPU run loop (~550 lines).
- **proc_emulation.c/h** — /proc and /dev path interception for openat/readlinkat (~380 lines).
- **syscall_exec.c/h** — execve: ELF reload, interpreter loading, page table rebuild, vCPU restart (~310 lines).
- **fork_ipc.c/h** — clone/fork via posix_spawn + IPC state transfer (~740 lines).
- **stack.c/h** — Linux initial stack builder (argc/argv/envp/auxv).
- **shim.S** — EL1 kernel shim. Exception vectors, SVC→HVC forwarding, MMU enable.
- **vm.c** — Legacy proof-of-concept host driver (kept in archive/).

## Key Constraints

- **Apple HVF enforces W^X** even with SCTLR.WXN=0. Regions can't be both writable
  and executable simultaneously. Use RW for data, RX for code.
- **SCTLR RES1 bits must be set explicitly** — HVF returns default SCTLR=0x0.
  Use SCTLR_RES1 mask (0x30D01804) + desired bits.
- **MMU must be enabled during vCPU execution** — via HVC #4 from the shim,
  not before hv_vcpu_run(). Setting SCTLR.M=1 via hv_vcpu_set_sys_reg before
  start causes permission faults on first instruction fetch.
- **GUEST_IPA_BASE must be 0** — ELF binaries use absolute addresses from their
  link address (e.g., 0x400000). Non-zero IPA base causes translation faults.
- System registers CANNOT be set via MSR from guest (HCR_EL2.TSC=1 traps all
  MSR writes). Use HVC #4 to request host-side register writes.
- Guest page tables use 2MB block mappings by default. For regions needing
  mixed permissions (e.g., shared library .text RX + .data RW in one 2MB
  range), blocks are split into 512 × 4KB L3 page descriptors via
  `guest_split_block()`. This is triggered automatically by MAP_FIXED
  and mprotect when permissions differ from the existing block.
- Only use HV_SYS_REG_* constants from Hypervisor.framework for register IDs.

## Exception Vector Critical Rule

**Vector entry stubs for svc_handler MUST NOT clobber any GPR.** The Linux
syscall ABI preserves ALL registers except X0 across SVC #0. Musl/GCC rely
on this for scratch registers (X9-X15). If a vector entry writes to any GPR
(e.g., `mov x5, #offset`) before svc_handler saves registers, the saved
value is wrong and the EL0 caller's register state is corrupted after ERET.

Only `bad_exception` vectors may clobber X5 (they halt, so no preservation needed).

## HVC Protocol

| HVC # | Purpose | Registers |
|-------|---------|-----------|
| #0 | Normal exit | x0 = exit code |
| #2 | Bad exception | x0=ESR, x1=FAR, x2=ELR, x3=SPSR, x5=vector |
| #4 | Set sysreg | x0 = reg ID, x1 = value |
| #5 | Syscall forward | X0-X5=args, X8=syscall nr; on return X8=TLBI flag |

## Build

```
make hl          # build + codesign hl
make test-hello  # build and run assembly hello world
make test-all    # run full test suite (34 tests)
make clean       # remove _build/
```

Requires macOS with Apple Silicon, Hypervisor.framework entitlement, and
nix develop shell with aarch64-unknown-linux-musl cross toolchain.

## Dynamic Linking

hl supports dynamically-linked aarch64-linux ELF binaries via `--sysroot`:

```
hl --sysroot /path/to/musl-sysroot ./my-dynamic-program
```

How it works:
1. `elf_load()` parses PT_INTERP to find the interpreter path (e.g., `/lib/ld-musl-aarch64.so.1`)
2. The interpreter is loaded as ET_DYN at `INTERP_LOAD_BASE` (0x40000000)
3. `build_linux_stack()` passes `AT_BASE` (interpreter load address) and
   `AT_EXECFN` (argv[0]) in the auxiliary vector
4. Entry point is set to `interp_entry + load_base` (dynamic linker takes over)
5. `sys_openat()` transparently redirects absolute paths through the sysroot:
   when `--sysroot` is set, tries `<sysroot>/<path>` first for absolute paths

The sysroot is inherited by fork children via IPC state transfer.
`sys_execve` also loads the interpreter for dynamically-linked targets,
so tools that execve dynamic children (env, nice, nohup) work correctly.
`elf_resolve_interp()` in elf.c is shared between hl.c and syscall_exec.c.

**Known limitations:**
- `timeout` fails — it uses fork/clone to create a child process with a
  timer, and the forked child inherits the dynamic linker state but the
  fork+exec path has issues in the interpreter space.

## L3 Page Table Splitting

Apple HVF enforces W^X on page table entries: a single entry cannot be both
writable and executable. With 2MB L2 block descriptors, this means an entire
2MB region must be either RW or RX. Shared libraries have both .text (RX)
and .data (RW) segments that often fall within the same 2MB range.

Solution: `guest_split_block()` in guest.c converts a 2MB L2 block descriptor
into a table descriptor pointing to an L3 table with 512 × 4KB page entries.
Each 4KB page can then have independent permissions. This is triggered by:
- `sys_mmap` MAP_FIXED: when the fixed address lands in a block with different
  permissions (e.g., dynamic linker overlaying .data RW onto library .text RX)
- `sys_mprotect`: when changing permissions for a sub-block range (e.g., RELRO)

The `guest_update_perms()` function handles the full workflow: checking if a
block needs splitting, splitting it, then updating individual L3 page entries.
Whole-block permission changes are done in place without splitting.

mmap uses a gap-finding allocator that walks the sorted region array to find
free address space. PROT_EXEC allocations go to the RX region
(MMAP_RX_BASE=0x10000000), others to the RW region (MMAP_BASE=0x200000000).
The RW region starts at 8GB to match real Linux kernel address space layout
where mmap regions sit well above text/data/brk. Address hints are honored
when possible.
This ensures .text and .data land in different 2MB blocks when possible;
L3 splitting handles the cases where they share a block.

## Implemented Syscalls (~130 total)

**Basic I/O:**
write(64), read(63), readv(65), writev(66), openat(56), close(57),
ioctl(29) [TIOCGWINSZ, TCGETS, TCSETS], lseek(62), fstat(80),
newfstatat(79), pread64(67), pwrite64(68)

**Process/Memory:**
exit(93), exit_group(94), brk(214), mmap(222), munmap(215), mprotect(226),
madvise(233), set_tid_address(96), getpid(172), gettid(178), uname(160),
getuid(174), geteuid(175), getgid(176), getegid(177), getrandom(278),
clock_gettime(113), gettimeofday(169), nanosleep(101), clock_nanosleep(115),
rt_sigaction(134), rt_sigprocmask(135), umask(166)

**Filesystem:**
getcwd(17), chdir(49), fchdir(50), faccessat(48), readlinkat(78),
unlinkat(35), mkdirat(34), renameat2(276), getdents64(61), dup(23),
dup3(24), fcntl(25), pipe2(59), ftruncate(46), truncate(45),
statfs(43), fstatfs(44), statx(291), flock(32), close_range(436)

**File manipulation (Batch 1):**
mknodat(33), symlinkat(36), linkat(37), fchmod(52), fchmodat(53),
fchownat(54), fchown(55), utimensat(88), futex(98), set_robust_list(99),
sigaltstack(132)

**Process management:**
execve(221), execveat(281), clone(220), wait4(260),
setuid(146), setgid(144), setreuid(145), setregid(143),
setresuid(147), getresuid(148), setresgid(149), getresgid(150),
setpriority(140), getpriority(141)

**Process/system info (Batch 2):**
sched_getaffinity(123), getpgid(155), getgroups(158), getrusage(165),
prctl(167), getppid(173), sysinfo(179), prlimit64(261)

**I/O optimization + sync (Batch 3):**
fallocate(47), sendfile(71), sync(81), fsync(82), fdatasync(83),
sched_yield(124), copy_file_range(285), splice(76), vmsplice(75)

**Signals + I/O multiplexing (Batch 4):**
pselect6(72), ppoll(73), kill(129), tgkill(131), rt_sigsuspend(133),
rt_sigpending(136), rt_sigreturn(139), setpgid(154), setsid(157)

**Timers:**
setitimer(103), getitimer(102), timerfd_create(85), timerfd_settime(86),
timerfd_gettime(87)

**Extended ioctls:**
TCSETSW(0x5403), TCSETSF(0x5404), TIOCGPGRP(0x540F), TIOCSPGRP(0x5410),
TIOCSCTTY(0x540E), FIONREAD(0x541B), TIOCNOTTY(0x5422), TIOCGSID(0x5429)

**xattr syscalls:**
getxattr(8), lgetxattr(9), setxattr(5), lsetxattr(6),
listxattr(11), llistxattr(12), removexattr(14), lremovexattr(15),
fgetxattr(16), fsetxattr(7), flistxattr(13), fremovexattr(18)

**Filesystem (additional):**
chroot(51), memfd_create(279)

**/proc and /dev emulation (intercepted in openat/readlinkat):**
/proc/self/exe, /proc/self/cwd, /proc/self/fd/N, /proc/cpuinfo,
/proc/self/status, /proc/self/maps, /proc/uptime, /proc/loadavg,
/var/run/utmp, /run/utmp, /dev/null, /dev/zero,
/dev/urandom, /dev/random, /dev/tty, /dev/stdin, /dev/stdout,
/dev/stderr, /dev/fd/N

**Stubs (return 0 / no-op but safe):**
mlock(228), munlock(229), msync(227), membarrier(283)

**Networking (syscall_net.c):**
socket(198), socketpair(199), bind(200), listen(201), accept(202),
connect(203), getsockname(204), getpeername(205), sendto(206),
recvfrom(207), setsockopt(208), getsockopt(209), shutdown(210),
sendmsg(211), recvmsg(212), accept4(242)

**epoll (emulated via kqueue):**
epoll_create1(20), epoll_ctl(21), epoll_pwait(22)

**Process management (additional):**
waitid(95)

**inotify (emulated via kqueue EVFILT_VNODE):**
inotify_init1(26), inotify_add_watch(27), inotify_rm_watch(28)

**Stubs (return -ENOSYS):**
tee(77), mincore(232), clone3(435)

**Stubs (return -EPERM):**
sethostname(161)

## Signal Delivery

Signals are fully implemented in `syscall_signal.c/.h`. The signal frame
matches Linux `arch/arm64/kernel/signal.c:setup_rt_frame()` layout so that
musl's `__restore_rt` → `rt_sigreturn` (SYS 139) correctly restores state.

Key points:
- `signal_deliver()` builds `linux_rt_sigframe_t` on guest stack, redirects
  vCPU PC to handler, sets X0=signum, X30=sa_restorer
- `signal_rt_sigreturn()` restores all 31 GPRs + SP + PC + PSTATE from frame
- `signal_reset_for_exec()` resets handlers to SIG_DFL on execve (POSIX:
  SIG_IGN stays SIG_IGN, pending/blocked preserved). Called from sys_execve
  after guest_reset
- SIGPIPE queued automatically when write/writev/pwrite64 returns EPIPE
- Guest ITIMER_REAL is emulated internally (not forwarded to host setitimer)
  because macOS shares alarm() and setitimer(ITIMER_REAL) as the same timer,
  and hl needs alarm() for its per-iteration vCPU timeout
- `signal_check_timer()` called from vCPU loop after each syscall
- After SYSCALL_EXEC_HAPPENED, vCPU loop verifies ELR_EL1 is non-zero
  (defensive check against HVF register sync bugs)

## Fork/Clone Architecture

macOS HVF allows only one VM per process. Fork is implemented via:
1. Parent creates socketpair(AF_UNIX, SOCK_STREAM)
2. Parent posix_spawn()s new `hl --fork-child <fd>` process
3. Parent serializes VM state over IPC: header + registers + memory
   regions (only used regions, not full 4GB) + FD table (via SCM_RIGHTS)
   + cwd + umask + signal state + shim blob + sentinel
4. Child receives state, creates own VM, restores registers directly
   into EL0 (bypasses shim _start to preserve callee-saved GPRs),
   enters vCPU loop with X0=0 (child return from clone)
5. Parent records child in process table, returns child PID

CLOEXEC semantics follow POSIX: all FDs (including CLOEXEC) are inherited
across fork. CLOEXEC only takes effect at exec (syscall_exec.c step 4).

## Key errno Translation

macOS and Linux errno values diverge starting around 35. The linux_errno()
function translates via switch statement. Notable mappings:
- macOS EAGAIN(35) → Linux EAGAIN(11)
- macOS ENOSYS(78) → Linux ENOSYS(38)
- macOS ENAMETOOLONG(63) → Linux ENAMETOOLONG(36)
- macOS ELOOP(62) → Linux ELOOP(40)

## AT_* Flag Translation

Linux and macOS AT_SYMLINK_NOFOLLOW values differ:
- Linux AT_SYMLINK_NOFOLLOW = 0x100, macOS = 0x20
- Linux AT_SYMLINK_FOLLOW = 0x400, macOS = 0x40
- Linux AT_REMOVEDIR = 0x200, macOS = 0x80
All AT_* flags must go through translate_at_flags() before macOS calls.

## aarch64-linux Open Flag Values

These differ from x86_64! From asm-generic/fcntl.h:
- O_DIRECTORY = 0x4000 (040000 octal)
- O_NOFOLLOW  = 0x8000 (0100000 octal)
- O_DIRECT    = 0x10000 (0200000 octal)
- O_LARGEFILE = 0x20000 (0400000 octal, no-op on LP64)
- O_CLOEXEC   = 0x80000 (02000000 octal)

## Linux vs macOS Clock IDs

Must translate! Linux CLOCK_MONOTONIC=1 but macOS CLOCK_MONOTONIC=6.
See translate_clockid() in syscall.c.

## Stack Alignment

The Linux initial stack must have SP 16-byte aligned AND pointing directly
at argc. The alignment padding must go ABOVE the structured area (before
auxv), not after pushing argc. Total entries = 33 + argc + envc; if odd,
push one padding word before auxv. Post-push masking (`sp &= ~15`) breaks
because it creates a gap between SP and argc.

## mmap Notes

MAP_SHARED is treated as MAP_PRIVATE (copy-on-write). Since the guest
is single-process, shared vs private semantics are equivalent. This
enables tools like `sort` on large files that use file-backed shared
mappings.

## Memory Layout

```
0x000010000  - 0x0000FFFFF:  Page table pool (960KB)
0x000100000  - 0x0001FFFFF:  Shim code (2MB block, RX)
0x000200000  - 0x0003FFFFF:  Shim data/stack (2MB block, RW)
0x000400000  - varies:        ELF LOAD segments (PIE_LOAD_BASE for ET_DYN)
0x001000000:                  brk base (16MB)
0x007E00000  - 0x007E00FFF:  Stack guard page (PROT_NONE, catches overflow)
0x007E01000  - 0x007FFFFFF:  Stack (2MB block, RW, grows down from 0x08000000)
0x010000000  - 0x01FFFFFFF:  mmap RX region (initial 256MB, pre-mapped RX)
0x020000000  - mmap_limit:    mmap RX growth area (up to g->mmap_limit)
0x200000000  - 0x20FFFFFFF:  mmap RW region (initial 256MB at 8GB, pre-mapped RW)
0x210000000  - mmap_limit:   mmap RW growth area (56GB@36-bit / 1016GB@40-bit)
interp_base  - varies:        Dynamic linker (g->interp_base, if --sysroot)
```

The address space size is determined at runtime by querying the max IPA
(Intermediate Physical Address) size via `hv_vm_config_get_max_ipa_size()`:
- **36-bit IPA (64GB)**: HVF default, mmap_limit=56GB, interp_base=60GB
- **40-bit IPA (1TB)**: macOS 15+, mmap_limit=1016GB, interp_base=1020GB

Both `mmap_limit` and `interp_base` are computed dynamically from
`guest_size` and stored in `guest_t` (replacing the old compile-time
`MMAP_END` and `INTERP_LOAD_BASE` constants). macOS demand-pages physical
memory on first touch, so only used pages consume RAM. The mmap RW region
starts at 8GB to match real Linux kernel address space layout. Additional
2MB blocks are mapped dynamically by `guest_extend_page_tables()` when
sys_mmap/sys_brk exceeds the current limit. The shim flushes the TLB
(via TLBI VMALLE1IS) when X8 is set non-zero after HVC #5 return.

For >512GB address spaces, the L0 page table needs multiple entries (each
covering 512GB). The page table functions (`guest_build_page_tables`,
`guest_extend_page_tables`, `find_l2_entry`) compute L0 index from the
actual IPA and allocate L1 tables on demand per L0 slot.

## Dynamic Page Table Extension (TLBI Protocol)

When sys_mmap or sys_brk needs memory beyond the currently-mapped page table
range, the host calls guest_extend_page_tables() to add new L2 entries.
This is safe because the vCPU is paused during HVC #5 handling. After
modification, the host sets X8=1 and g->need_tlbi=0. The shim checks X8
after HVC #5: if non-zero, it executes `TLBI VMALLE1IS; DSB ISH; ISB`
before ERET. X8 (syscall number register) is clobbered by the Linux ABI,
so callers never expect it preserved.

**IMPORTANT: SYSCALL_EXEC_HAPPENED bypasses X8 TLBI logic.** When
sys_execve returns SYSCALL_EXEC_HAPPENED, it bypasses the normal
syscall_dispatch() epilogue that sets X8 for TLBI. Therefore, sys_execve
sets X8=1 directly via hv_vcpu_set_reg() — do NOT rely on g->need_tlbi
for exec. Any future code path that returns SYSCALL_EXEC_HAPPENED and
needs TLBI must set X8 explicitly.

## Socket Networking

Socket syscalls are translated in `syscall_net.c/.h`. Key translations:
- **AF_INET6**: Linux=10, macOS=30
- **sockaddr**: Linux has no `sa_len` byte; macOS does. All sockaddr
  conversions go through `linux_to_mac_sockaddr()`/`mac_to_linux_sockaddr()`
- **Socket type flags**: Linux OR's SOCK_NONBLOCK(0x800) and
  SOCK_CLOEXEC(0x80000) into the type argument; must extract before socket()
- **SOL_SOCKET options**: SO_TYPE, SO_SNDBUF, SO_RCVBUF etc. have different
  numeric values on Linux vs macOS

## Multi-threading Architecture

Fully implemented. Guest threads map 1:1 to host pthreads, each with its
own HVF vCPU. Three test suites validate correctness: `test-thread` (basic
clone/futex), `test-pthread` (musl pthread_create/join/mutex), and
`test-signal-thread` (per-thread signal masks).

### HVF Multi-vCPU Support

Apple's Hypervisor.framework supports multiple vCPUs per VM. Each vCPU is
bound to the host thread that created it. Multiple vCPUs share the same
guest physical memory via `hv_vm_map()`. Validated by `test/test-multi-vcpu.c`
(5/5 tests pass). Run with `make test-multi-vcpu`.

### Implementation

**Thread table** (`thread.c/h`):
- `thread_entry_t` per thread: vCPU handle, host pthread, per-thread
  signal mask, `clear_child_tid` for CLONE_CHILD_CLEARTID, `sp_el1`
- `_Thread_local current_thread` for O(1) access from syscall handlers
- MAX_THREADS = 64 concurrent guest threads per VM
- SP_EL1 allocation: each thread gets a 4KB EL1 exception stack from
  the shim data region

**Futex** (`futex.c/h`):
- Hash table of wait queues keyed by guest virtual address
- 7 operations: FUTEX_WAIT, FUTEX_WAKE, FUTEX_WAIT_BITSET,
  FUTEX_WAKE_BITSET, FUTEX_REQUEUE, FUTEX_CMP_REQUEUE, FUTEX_WAKE_OP
- Per-waiter condition variables for precise wakeup
- `futex_wake_one()` used by thread exit for CLONE_CHILD_CLEARTID

**sys_clone with CLONE_THREAD** (`fork_ipc.c`):
1. `hv_vcpu_create()` + per-thread SP_EL1 allocation
2. Set child SP, TPIDR_EL0 (TLS), copy parent's signal mask
3. `pthread_create()` running `vcpu_run_loop()` for child vCPU
4. Return child TID to parent, child runs with X0=0

**Thread-safety locks** (across files):

| Resource | Lock type | File |
|----------|-----------|------|
| mmap/brk allocators + page tables | pthread_mutex | syscall.c |
| FD table | pthread_mutex | syscall.c |
| Thread table | pthread_mutex | thread.c |
| Futex wait queues | pthread_mutex (per-bucket) | futex.c |

**exit_group** (`syscall.c`):
- Sets global `exit_group_requested` flag
- `thread_for_each(thread_force_exit_cb)` — calls `hv_vcpus_exit()`
  on all worker vCPUs to break them out of `hv_vcpu_run()`
- Joins worker threads with timeout to allow CLEARTID cleanup

### Implementation Notes

0. **guest_t vs per-thread state**: Solved with a separate `thread_entry_t`
   table alongside `guest_t`. Syscall handlers use `current_thread->vcpu`
   instead of `g->vcpu`. The `guest_t` struct holds shared VM state (memory,
   page tables, regions); per-thread state lives in the thread table.

1. **Futex atomicity**: Hash-bucket mutex is held during compare-and-wait.
   The guest futex word is read while holding the bucket lock, then the
   waiter is enqueued atomically before releasing the lock.

2. **Page table consistency**: mmap_lock serializes all page table
   modifications. TLBI broadcasts via `TLBI VMALLE1IS` from any vCPU
   invalidate all others (hardware coherency verified by test-multi-vcpu).

3. **Per-thread signal masks**: Each `thread_entry_t` has its own `blocked`
   mask. `rt_sigprocmask` operates on `current_thread->blocked`. Child
   threads inherit the parent's mask at clone time.

4. **CLONE_CHILD_CLEARTID**: On thread exit, worker writes 0 to
   `clear_child_tid` GVA and calls `futex_wake_one()` — this is how
   `pthread_join()` works via the TID address.

### Not Implemented

- Robust futexes (set_robust_list) — stub returns 0; cleanup on crash
- PI futexes (priority inheritance) — real-time only
- CPU affinity (sched_setaffinity) — returns all-CPUs mask
- clone3 — returns -ENOSYS; musl falls back to clone()
