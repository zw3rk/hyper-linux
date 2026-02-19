# hyper-linux

Run static aarch64-linux ELF binaries on macOS Apple Silicon via Hypervisor.framework.

## Architecture

- **hl.c** — Main entry point. CLI (incl. --fork-child), VM setup (~400 lines).
- **guest.c/h** — Guest memory management. Page tables, read/write, brk/mmap, reset.
- **elf.c/h** — ELF64 parser and loader for static aarch64-linux binaries.
- **syscall.c/h** — Core infrastructure (FD table, errno/flag translation, brk/mmap) + dispatch switch (~900 lines).
- **syscall_internal.h** — Shared declarations for syscall module helpers.
- **syscall_fs.c/h** — Filesystem: stat, open, close, directory, xattr, permissions (~960 lines).
- **syscall_io.c/h** — I/O: read/write, ioctl, splice, sendfile, poll/select (~610 lines).
- **syscall_time.c/h** — Time: clock_gettime, nanosleep, gettimeofday, setitimer (~190 lines).
- **syscall_sys.c/h** — System info: uname, getrandom, sysinfo, prlimit64 (~240 lines).
- **syscall_signal.c/h** — Signal delivery: rt_sigframe, rt_sigaction, delivery, ITIMER_REAL (~520 lines).
- **syscall_net.c/h** — Socket networking: AF/sockaddr/sockopt translation (~670 lines).
- **syscall_proc.c/h** — Process syscalls: execve, clone/fork (IPC), wait4, vCPU loop (~1520 lines).
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
- Guest page tables MUST use 2MB block mappings for misaligned access support.
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
make test-all    # run full test suite (19 tests)
make clean       # remove _build/
```

Requires macOS with Apple Silicon, Hypervisor.framework entitlement, and
nix develop shell with aarch64-unknown-linux-musl cross toolchain.

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
setitimer(103), getitimer(102)

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

**Stubs (return 0):**
mlock(228), munlock(229), msync(227)

**Networking (syscall_net.c):**
socket(198), socketpair(199), bind(200), listen(201), accept(202),
connect(203), getsockname(204), getpeername(205), sendto(206),
recvfrom(207), setsockopt(208), getsockopt(209), shutdown(210),
sendmsg(211), recvmsg(212), accept4(242)

**Stubs (return -ENOSYS):**
tee(77), timerfd_create(85), timerfd_settime(86), timerfd_gettime(87),
epoll_create1(20), epoll_ctl(21), epoll_pwait(22),
inotify_init1(26), inotify_add_watch(27), inotify_rm_watch(28),
waitid(95), mincore(232), clone3(435)

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
- SIGPIPE queued automatically when write/writev/pwrite64 returns EPIPE
- Guest ITIMER_REAL is emulated internally (not forwarded to host setitimer)
  because macOS shares alarm() and setitimer(ITIMER_REAL) as the same timer,
  and hl needs alarm() for its per-iteration vCPU timeout
- `signal_check_timer()` called from vCPU loop after each syscall

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
0x00010000  - 0x000FFFFF:   Page table pool (960KB)
0x00100000  - 0x001FFFFF:   Shim code (2MB block, RX)
0x00200000  - 0x003FFFFF:   Shim data/stack (2MB block, RW)
0x00400000  - varies:        ELF LOAD segments
0x01000000:                  brk base (16MB)
0x07E00000  - 0x07FFFFFF:   Stack (2MB block, RW, grows down from 0x08000000)
0x10000000  - 0x1FFFFFFF:   mmap region (initial 256MB, pre-mapped RW)
0x20000000  - 0xFFFFFFFF:   mmap growth area (dynamically mapped on demand)
```

Total: 4GB address space reserved via mmap(MAP_ANON). macOS demand-pages
physical memory on first touch, so only used pages consume RAM. The initial
512MB region is pre-mapped in page tables; additional 2MB blocks are mapped
dynamically by guest_extend_page_tables() when sys_mmap/sys_brk exceeds the
current limit. The shim flushes the TLB (via TLBI VMALLE1IS) when X8 is
set non-zero after HVC #5 return.

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
