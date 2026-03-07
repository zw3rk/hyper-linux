# hyper-linux

Run static and dynamically-linked aarch64-linux and x86_64-linux ELF binaries on macOS Apple Silicon via Hypervisor.framework.

## Architecture

All source lives under `src/`.  Build with `make hl` (sources found via `-Isrc`).

- **src/hl.c** — Main entry point. CLI (incl. --fork-child, --sysroot), VM setup, interpreter loading (~500 lines).
- **src/guest.c/h** — Guest memory management. Page tables, read/write, brk/mmap, reset, region tracking.
- **src/elf.c/h** — ELF64 parser/loader. PT_LOAD segments, PT_INTERP parsing, ET_DYN load_base support.
- **src/syscall.c/h** — Core infrastructure (FD table, errno/flag translation, brk/mmap) + dispatch switch (~900 lines).
- **src/syscall_internal.h** — Shared declarations for syscall module helpers.
- **src/syscall_fs.c/h** — Filesystem: stat, open, close, directory, xattr, permissions (~960 lines).
- **src/syscall_io.c/h** — I/O: read/write, ioctl, splice, sendfile, poll/select (~610 lines).
- **src/syscall_inotify.c/h** — inotify emulation via kqueue EVFILT_VNODE (~350 lines).
- **src/syscall_time.c/h** — Time: clock_gettime, nanosleep, gettimeofday, setitimer (~190 lines).
- **src/syscall_sys.c/h** — System info: uname, getrandom, sysinfo, prlimit64 (~240 lines).
- **src/syscall_signal.c/h** — Signal delivery: rt_sigframe, rt_sigaction, delivery, ITIMER_REAL (~520 lines).
- **src/syscall_net.c/h** — Socket networking: AF/sockaddr/sockopt translation (~670 lines).
- **src/syscall_proc.c/h** — Process state, accessors, wait4/waitid, vCPU run loop (~550 lines).
- **src/proc_emulation.c/h** — /proc and /dev path interception for openat/readlinkat (~380 lines).
- **src/syscall_exec.c/h** — execve: ELF reload, interpreter loading, page table rebuild, vCPU restart (~310 lines).
- **src/fork_ipc.c/h** — clone/fork via posix_spawn + IPC state transfer (~740 lines).
- **src/crash_report.c/h** — Structured crash report for GitHub issue filing (~250 lines).
- **src/stack.c/h** — Linux initial stack builder (argc/argv/envp/auxv).
- **src/shim.S** — EL1 kernel shim. Exception vectors, SVC→HVC forwarding, MMU enable.

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

## DC ZVA Emulation (Critical for Rosetta)

HVF traps DC ZVA (Data Cache Zero by VA) via HCR_EL2.TDZ=1 even with
SCTLR_EL1.DZE=1. The shim MUST emulate DC ZVA by zeroing 64 bytes at
the cache-line-aligned address from the Rt register. Without this,
rosetta's JIT compiler produces corrupted block tables (stale data in
memory that DC ZVA was supposed to zero), causing assertion failures.

## HVC Protocol

| HVC # | Purpose | Registers |
|-------|---------|-----------|
| #0 | Normal exit | x0 = exit code |
| #2 | Bad exception | x0=ESR, x1=FAR, x2=ELR, x3=SPSR, x5=vector |
| #4 | Set sysreg | x0 = reg ID, x1 = value |
| #5 | Syscall forward | X0-X5=args, X8=syscall nr; on return X8=TLBI flag |

## Build

Source files live in `src/`, build artifacts in `_build/`.

```
make hl          # build + codesign hl
make test-hello  # build and run assembly hello world
make test-all    # run full test suite
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
`elf_resolve_interp()` in src/elf.c is shared between src/hl.c and src/syscall_exec.c.

**Known limitations:**
- `timeout` fails — it uses fork/clone to create a child process with a
  timer, and the forked child inherits the dynamic linker state but the
  fork+exec path has issues in the interpreter space.

## x86_64-linux via Rosetta

hl supports x86_64-linux ELF binaries via Apple's Rosetta Linux translator:

```
hl ./my-x86_64-binary [args...]
```

How it works:
1. `elf_load()` detects `e_machine == EM_X86_64` (62)
2. Rosetta binary loaded from `/Library/Apple/usr/libexec/oah/RosettaLinux/rosetta`
   (static aarch64-linux ELF, ET_EXEC, linked at 0x800000000000 / 128TB)
3. HVF VM created with 48-bit IPA to cover rosetta's link address
4. Rosetta mapped at high IPA via `guest_add_mapping()` (separate host buffer
   because macOS user space is <128TB, so can't identity-map at that address)
5. x86_64 binary mapped at its requested address in the primary buffer (low IPA)
6. Entry point set to rosetta's entry; argv follows binfmt_misc convention:
   `[rosetta_path, x86_64_binary_path, original_argv...]`
7. Rosetta JIT-translates x86_64→ARM64 at runtime; all syscalls appear as
   ARM64 SVC #0 — hl's existing ~140 syscall handlers work transparently

### Multi-Region Guest Memory

The primary buffer (host_base at IPA 0) covers up to 1TB (40-bit).
High-IPA regions (like rosetta at 128TB) use `guest_add_mapping()`:
- Allocates a demand-paged host buffer via mmap at a low host address
- Maps it via `hv_vm_map()` at the requested high IPA
- `gva_resolve()` handles multi-region address translation (fast path
  for primary region, linear scan for extra mappings)
- Page table builder handles arbitrary GPAs naturally (L0 index 256
  for rosetta at 128TB, 512GB per L0 entry)

### 48-bit IPA

Apple Silicon supports up to 48-bit IPA despite `hv_vm_config_get_max_ipa_size()`
reporting 36 as default. The IPA width is requested via:
```c
hv_vm_config_set_ipa_size(config, 48);
```
Only requested when rosetta is needed; normal aarch64 binaries use the
standard 36/40-bit path.

### Rosetta Stack/Auxv Semantics

Rosetta is treated as a binfmt_misc interpreter (NOT a PT_INTERP dynamic linker):
- Auxv (AT_PHDR/PHENT/PHNUM/ENTRY) describes rosetta, not the x86_64 binary
- Rosetta discovers the x86_64 binary via argv[1] (the real path)
- PT_INTERP from the x86_64 binary is skipped — rosetta handles x86_64
  dynamic linking internally (loading ld-linux-x86-64.so.2 etc.)
- AT_PLATFORM remains "aarch64" (correct: rosetta produces ARM64 code)

### x86_64 musl AT_BASE Fix

Rosetta reads AT_BASE from its own (host) auxv as a template when constructing
the x86_64 auxv for dynamically-linked binaries. The Linux kernel always emits
AT_BASE (value=0 for static binaries). hl's `build_linux_stack()` previously
omitted AT_BASE when `interp_base == 0` (static binaries like Rosetta), causing
Rosetta to skip AT_BASE in the x86_64 auxv. musl's `_dlstart_c` then fell back
to scanning AT_PHDR for PT_DYNAMIC, found the wrong base → SIGFPE.

Fix in `src/stack.c`: always emit `AT_BASE` (matching Linux kernel behavior).
This lets Rosetta copy the entry and fill in the correct interpreter address.
glibc is unaffected (uses `__ehdr_start` for base computation, independent of
AT_BASE).

### Rosetta AOT Translation (rosettad)

Rosetta supports ahead-of-time (AOT) translation via the `rosettad` daemon,
which is also a static aarch64-linux binary. AOT translation produces
ARM64 code for the x86_64 binary's functions.

**Rosetta TranslationMode Architecture** (reverse-engineered from the binary):

Rosetta has three independent mode concepts:
1. **BSS[0x474] "translation quality"**: Hardware-dependent, set at init.
   1=basic (M2, no FEAT_AFP), 2=enhanced (M3+, has FEAT_AFP). Set by
   checking ID_AA64MMFR1_EL1 bits 44-47 (AFP field) at VA 0x800000030c64.
2. **TranslationMode enum** (per-translation-unit): 0=Aot, 1=JIT, 2=AOT-assisted JIT, 3=unknown.
   Stored at object offset 0x18 and stack local [sp,#0x110]/[sp,#0xb4].
3. **is_aot_mode flag** (per-fragment): Object offset 0x214. Extracted from
   bit 8 of packed AOT metadata at fragment header offset 0x88. The packed
   field is computed by function at 0x54bf4 from two source uint64 values
   at [source+0x80] and [source+0x88]: bit 8 of packed = bit 4 of
   [source+0x88]. When set, the translate caller forces mode=2
   (AOT-assisted JIT). **Currently always 0** because the AOT files from
   `rosettad translate` have zeros at offsets 0x80 and 0x88.

**Mode computation** (at VA 0x800000034f78-0x800000034f94):
```
and w8, w8, #0xff    ; mask is_aot_mode
cmp w8, #1           ; compare with 1
mov w8, #1           ; default = JIT
cinc w8, w8, eq      ; if is_aot_mode == 1: mode=2 (AOT-assisted)
```
The JIT translate path NEVER uses TranslationMode::Aot (0). It only
produces mode 1 (pure JIT) or mode 2 (AOT-assisted JIT).

**TranslationMode::Aot (0) is a completely separate code path** used by
TranslationCacheAot.cpp for fully pre-translated AOT cache entries. It
uses the dispatch at VA 0x800000039e00 (Translator.cpp) and supports
dyld stub translation (translate_indirect_jmp_dyld_stub at 0x39ed0,
which asserts _mode==Aot). Mode 0 is set by a lookup function at
0x29ae4 that searches a table at 0x80000001c380.

**How AOT-assisted JIT (mode 2) prevents block_for_offset failures:**
- At init (0x46ee8): mode 2 calls `seed_block()` at 0x2a7b0, which
  pre-registers all blocks from AOT data into the block table
- In the main translate loop (0x47cdc): mode 2 takes a short path
  (`b.eq 0x47f8c`) that skips the block list iteration at 0x47ce8-0x47d3c
- `block_for_offset()` at 0x387d4 uses binary search on the block table;
  with AOT-seeded blocks, lookups succeed instead of returning NULL

**The "BasicBlock requested for unrecognized address" assertion** at
VA 0x800000038838 fires in `block_for_offset` (BuilderBase.h:550) when
the binary search returns NULL. This happens for indirect jumps to
addresses not pre-registered as block starts. In mode 2, AOT data
pre-populates all known blocks; in mode 1, only discovered blocks exist.

**Root cause of x86_64 indirect jump failures (RESOLVED):** DC ZVA (Data
Cache Zero by VA) was trapped by HVF (HCR_EL2.TDZ=1) but not emulated.
The shim only counted the trap and ran IC IALLU without zeroing memory.
Rosetta uses DC ZVA as fast memset(0) for JIT code buffers and internal
metadata. Without actual zeroing, stale data corrupted rosetta's block
tables, causing `block_for_offset()` to fail on indirect jump targets.

**Fix:** The shim now emulates DC ZVA by decoding the ISS to identify the
instruction (Op0=1,Op1=3,CRn=7,CRm=4,Op2=1), extracting the Rt register
from ISS[9:5], loading the VA from the saved register frame, and zeroing
64 bytes at the cache-line-aligned address using four STP xzr pairs.
SCTLR_EL1.DZE (bit 14) is also set to allow DC ZVA at EL0.

**Previous incorrect theories (now disproven):**
- AOT `is_aot_mode` flag / `seed_block()` pre-population: not the cause
- Rosetta binary patching (TranslationMode, is_aot_mode): wrong approach
- BSS signal handler pre-population: incorrect (BSS stores OLD handler)
- rosettad AOT metadata at offsets 0x80/0x88: unrelated to the crash

**AOT file format** (from `rosettad translate` output):
```
Offset  Size  Field
0x00    8     total_size (mapped code+data region size)
0x08    8     version (always 1)
0x10    8     orig_size (original x86_64 binary mapped size)
0x18    8     code_offset (file offset of translated ARM64 code, always 0x1000)
0x20    8     unknown
0x28    8     unknown
0x30    8     unknown
0x38    8     metadata_offset
0x40    8     metadata_end
0x48    8     block_table_end
0x50    4     code_align (0x1000)
0x54    4     entry_count (number of translated entry points)
0x58-0xFFF    zero padding
0x1000+       translated ARM64 code
```
Offsets 0x80 and 0x88 (the `is_aot_mode` source fields) are always zero
in standalone `rosettad translate` output. hl patches bit 4 of the uint64
at offset 0x88 after rosettad translate (in `patch_aot_is_aot_mode()`,
src/syscall_io.c) to enable AOT-assisted JIT mode and `seed_block()`
pre-population. Additionally, 3 UBFX extraction sites in the rosetta
binary are patched to `mov wN, #1` (in src/hl.c) to force is_aot_mode=1
regardless of AOT file contents.

### I-cache / D-cache Coherence (IC IALLU)

ARM64 I-cache and D-cache are not automatically coherent. When rosetta's
JIT signal handler writes translated code (patching BRK stubs with branch
instructions), the D-cache is updated but the I-cache retains the old BRK
instruction. Without explicit I-cache invalidation, the CPU fetches the
stale BRK from the I-cache, causing infinite re-traps.

The shim executes `IC IALLU; DSB ISH; ISB` after every syscall return
(post-HVC #5 in handle_svc_0). This ensures that rt_sigreturn from
rosetta's signal handler flushes the I-cache before the translated code
executes. IC IALLU is a single-cycle operation on Apple Silicon; the
ISB pipeline flush (~10 cycles) is negligible vs HVC overhead (~1000+).

Rosetta's binary contains only 1 IC IVAU instruction (at VA 0x2686c)
which is never called in the JIT path — it relies on external cache
maintenance, which the shim now provides.

### Rosetta JIT Translation Status

Simple x86_64 binaries (hello-write, echo) run successfully through
rosetta's AOT-assisted JIT path. Complex binaries with indirect jumps
(printf jump tables, .init_array function pointers) still fail:

1. First BRK stub: translated and patched successfully (IC IALLU works)
2. Subsequent BRK: rosetta's signal handler detects a translation failure
   (block_for_offset returns NULL for indirect jump targets)
3. Handler calls rt_sigaction(SIGTRAP, SIG_DFL) to reset, then
   rt_tgsigqueueinfo to re-raise SIGTRAP with SIG_DFL (intentional
   "give up and die" pattern)
4. Process terminates cleanly with SIGTRAP default disposition

This is a genuine rosetta limitation also observed in real VZ VMs
(OrbStack #1396, Docker #7320). The block_for_offset assertion fires
for addresses not pre-registered as block starts, even with AOT data.

**The `_potential_targets` assertion** at VA 0x800000048eac
(`_mode == TranslationMode::Aot || _potential_targets.empty()`,
BranchTargetFinderBase.hpp:57) is a defensive check. Within the big
translate function, `_potential_targets` ([sp,#0x150-0x160]) is only
ever initialized to zero (0x46e54) and reset to zero (0x4758c). The
assertion guards against future bugs where potential targets might leak
into non-Aot modes.

**BSS[0xa04] is the VZ enable flag** — written at VA 0x800000030304
from caps[0] of the VZ_CAPS ioctl result. When VZ is active (our hl
sets caps[0]=1), BSS[0xa04]=1, and the runtime AOT code at 0x90afc
proceeds to connect to rosettad via socket.

**BSS[0xa05]** is a secondary VZ capability flag from caps[108].
Must be non-null to allow the rosettad translate path (checked at VA 0x90ba4).
**BSS[0xa06..0xa71]** (108 bytes) stores the `sun_path` socket path bytes,
copied from VZ_CAPS caps[1..108]. NOTE: caps[64] (sun_path[63]) MUST be
non-null — it gates the entire rosettad AOT initialization (VA 0x307a4).
caps[66..] (sun_path[65..]) holds the null-terminated x86_64 binary path
that Rosetta passes to rosettad for AOT translation.

**VZ mode activation:** Rosetta checks for VZ (Virtualization.framework)
support via 3 ioctls: VZ_CHECK (0x80456125) returns a 69-byte signature,
VZ_CAPS (0x80806123) returns 128 bytes of capability data, VZ_ACTIVATE
(0x6124). These are intercepted in `syscall_io.c`.

**VZ_CAPS buffer layout** (128 bytes, reverse-engineered from Rosetta binary):
- `caps[0]`: VZ enable flag (1 = active). Written to BSS[0xa04].
- `caps[1..64]`: `sun_path[0..63]` — first 64 bytes of socket path.
  **`caps[64]` (= `sun_path[63]`) MUST be non-null.** At VA 0x800000307a4,
  Rosetta reads this byte and branches (`cbz w8, 0x3080c`): if zero, the
  entire rosettad AOT initialization is skipped silently. `[aot_registry+0x70]`
  is never populated, and the AOT mmap function (0x80000002ec84) always
  returns without contacting rosettad. We fill caps[1..64] with 63 `'A'`
  bytes + `'/'` as a fake path prefix; the connect() interception is fd-based
  so the actual bytes don't matter.
- `caps[65]`: Null terminator of the socket path (zero).
- `caps[66..107]`: Null-terminated path to the x86_64 binary being translated
  (`caps+0x42` in Rosetta notation). At VA 0x800000090bb0, Rosetta calls
  `openat(AT_FDCWD, caps+0x42, 0, 0)` to open the binary and sends the fd
  to rosettad via SCM_RIGHTS for AOT translation.
- `caps[108]`: Written to BSS[0xa05]. Checked at VA 0x800000090ba4
  (`cbz w23, 0x90ca4`): must be non-null to allow the translate (`'t'`)
  handler to proceed after the `'?'` handshake. We ensure it's non-null
  (set to 1 if not already non-zero from the binary path bytes).
- `caps[109..127]`: Additional capability flags (purpose unknown, left as zero)

Our connect() interception in `syscall_net.c` is fd-based (`rosettad_is_socket()`),
so Rosetta never reaches the actual socket path bytes in caps[1..108]. The
rosettad protocol is handled by `rosettad_handler_thread` on the other end
of the socketpair.

**rosettad protocol** (implemented in `syscall_io.c:rosettad_handler_thread`):
- Rosetta opens `AF_UNIX SOCK_SEQPACKET` — intercepted in `syscall_net.c`
  with `socketpair(SOCK_STREAM)` (macOS doesn't support SEQPACKET for AF_UNIX)
- `'?'` → handshake, respond `0x01`
- `'t'` → translate: receive binary fd via SCM_RIGHTS, run `rosettad translate`
  via subprocess, send back AOT fd + 32-byte digest
- `'d'` → digest cache lookup (respond `0x00` = not cached)
- `'q'` → quit

**cmsghdr format translation** (critical for SCM_RIGHTS to work):
Linux aarch64 and macOS have incompatible `struct cmsghdr` layouts:
- Linux: `{uint64_t cmsg_len, int level, int type}` (16 bytes), data at offset 16,
  CMSG_ALIGN rounds to 8. SOL_SOCKET=1
- macOS: `{uint32_t cmsg_len, int level, int type}` (12 bytes), data at offset 12,
  CMSG_ALIGN rounds to 4. SOL_SOCKET=0xFFFF

`sys_sendmsg` translates Linux→macOS, `sys_recvmsg` translates macOS→Linux.
Both translate SOL_SOCKET values and guest↔host fd numbers for SCM_RIGHTS.

**argv construction:** Rosetta passes argv[1] directly as the translated
program's argv[0]. We pass the real binary path (not `/proc/self/fd/N`)
so programs like busybox can use `basename(argv[0])` for applet lookup.

### Fork/IPC Propagation

`fork_ipc.c` propagates `g->ipa_bits`, rosetta placement fields, and kbuf
state in the IPC header so child processes create their VMs with the same
IPA width and rosetta configuration. Rosetta fork children use the legacy
IPC region-copy path (not COW) because rosetta's JIT state is process-local.
Extra mappings (rosetta segments) are re-created from the IPC header fields.

## L3 Page Table Splitting

Apple HVF enforces W^X on page table entries: a single entry cannot be both
writable and executable. With 2MB L2 block descriptors, this means an entire
2MB region must be either RW or RX. Shared libraries have both .text (RX)
and .data (RW) segments that often fall within the same 2MB range.

Solution: `guest_split_block()` in src/guest.c converts a 2MB L2 block descriptor
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

## Implemented Syscalls

~140 syscalls implemented.  See the **SYSCALL SUPPORT** section in `hl.1`
(the man page) for the complete, authoritative list.

## Signal Delivery

Signals are fully implemented in `src/syscall_signal.c/.h`. The signal frame
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
3. Parent serializes VM state over IPC (two paths, see below)
4. Child receives state, creates own VM, restores registers directly
   into EL0 (bypasses shim _start to preserve callee-saved GPRs),
   enters vCPU loop with X0=0 (child return from clone)
5. Parent records child in process table, returns child PID

### COW Fork Path (aarch64, IPC v4)

When `g->shm_fd >= 0` and `!g->is_rosetta`, guest memory is file-backed
(mkstemp+unlink, MAP_SHARED). Fork sends the backing fd via SCM_RIGHTS:
- Parent stays on MAP_SHARED (does NOT remap — HVF caches VA→PA)
- Child maps the fd MAP_PRIVATE → instant COW clone, zero data copy
- IPC header has `has_shm=1`, `num_regions=0` (skip memory serialization)
- Child calls `guest_init_from_shm()` instead of `guest_init()`
- ~50x faster than IPC copy path for large guest memory

**Critical constraints discovered:**
- macOS rejects MAP_PRIVATE on shm_open fds (EINVAL) — use mkstemp+unlink
- HVF caches host VA→PA from hv_vm_map; MAP_FIXED remap does NOT update
  Stage-2, causing vCPU to read stale pages. Parent must NOT remap host_base
- Child must restore `g->ttbr0` from IPC header (guest_init_from_shm zeroes
  the struct; without ttbr0, page table walks fail for all high VAs)

### Legacy IPC Copy Path (rosetta, fallback)

When `g->shm_fd < 0` or `g->is_rosetta`:
- Parent serializes used memory regions in 1MB chunks over the socketpair
- Child calls `guest_init()` and receives region data into fresh guest memory
- Rosetta JIT state (TLS, code caches, slab allocators) is process-local
  and corrupts when COW-copied — rosetta always uses this path

CLOEXEC semantics follow POSIX: all FDs (including CLOEXEC) are inherited
across fork. CLOEXEC only takes effect at exec (src/syscall_exec.c step 4).

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
See translate_clockid() in src/syscall.c.

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

**MAP_FIXED file-backed mmap** must pread() file contents into guest
memory. Both the MAP_FIXED and non-fixed paths need this. The MAP_FIXED
path zeros the region first (for pages beyond EOF), then overlays with
file data. This is critical for Rosetta's AOT loading — rosetta uses
MAP_FIXED to map AOT code/data sections from the translated file.

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
--- Extra IPA mappings (x86_64 mode only, via guest_add_mapping) ---
0x800000000000+:               Rosetta binary (3 segments at 128TB link addr)
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

Socket syscalls are translated in `src/syscall_net.c/.h`. Key translations:
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

**Thread table** (`src/thread.c/h`):
- `thread_entry_t` per thread: vCPU handle, host pthread, per-thread
  signal mask, `clear_child_tid` for CLONE_CHILD_CLEARTID, `sp_el1`
- `_Thread_local current_thread` for O(1) access from syscall handlers
- MAX_THREADS = 64 concurrent guest threads per VM
- SP_EL1 allocation: each thread gets a 4KB EL1 exception stack from
  the shim data region

**Futex** (`src/futex.c/h`):
- Hash table of wait queues keyed by guest virtual address
- 7 operations: FUTEX_WAIT, FUTEX_WAKE, FUTEX_WAIT_BITSET,
  FUTEX_WAKE_BITSET, FUTEX_REQUEUE, FUTEX_CMP_REQUEUE, FUTEX_WAKE_OP
- Per-waiter condition variables for precise wakeup
- `futex_wake_one()` used by thread exit for CLONE_CHILD_CLEARTID

**sys_clone with CLONE_THREAD** (`src/fork_ipc.c`):
1. `hv_vcpu_create()` + per-thread SP_EL1 allocation
2. Set child SP, TPIDR_EL0 (TLS), copy parent's signal mask
3. `pthread_create()` running `vcpu_run_loop()` for child vCPU
4. Return child TID to parent, child runs with X0=0

**Thread-safety locks** (across files):

| Resource | Lock type | File |
|----------|-----------|------|
| mmap/brk allocators + page tables | pthread_mutex | src/syscall.c |
| FD table | pthread_mutex | src/syscall.c |
| Thread table | pthread_mutex | src/thread.c |
| Futex wait queues | pthread_mutex (per-bucket) | src/futex.c |

**exit_group** (`src/syscall.c`):
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

## Ptrace and clone(CLONE_VM) for Rosetta JIT

Rosetta's two-process JIT architecture uses `clone(CLONE_VM)` to create
an inferior process that shares guest memory, then attaches via
`PTRACE_SEIZE`. BRK instructions in translated code trigger ptrace-stops,
allowing the tracer to read/write registers and discover untranslated
code on-demand.

### clone(CLONE_VM) — In-Process VM-Clone

`sys_clone_vm()` in `src/fork_ipc.c` handles `CLONE_VM` without
`CLONE_THREAD`. Unlike `sys_clone_thread()` (CLONE_THREAD), VM-clone
children are waitable via wait4 and have exit semantics (exit_signal,
vm_exit_status). Unlike the posix_spawn fork path, they share the
same `guest_t*` (same guest memory, page tables).

### sys_ptrace Operations

Implemented in `src/syscall_proc.c`:
- **PTRACE_SEIZE**: Attach to thread without stopping; sets `ptraced=1`
- **PTRACE_CONT**: Resume stopped tracee, optionally inject signal
- **PTRACE_INTERRUPT**: Force tracee into ptrace-stop via `hv_vcpus_exit()`
- **PTRACE_GETREGSET** (NT_PRSTATUS): Read tracee's register snapshot
- **PTRACE_SETREGSET** (NT_PRSTATUS): Write tracee's registers (applied on resume)

### Register Snapshot Protocol

Cross-thread HVF register access may not be supported, so we use a
snapshot protocol: the tracee snapshots its own vCPU registers into
`ptrace_regs` before entering ptrace-stop, and applies any dirty
changes back to the vCPU on resume. This ensures all HVF register
access happens on the owning thread.

### BRK + Ptrace-Stop Flow

1. Guest executes BRK → shim forwards via HVC #10
2. If `current_thread->ptraced`: tracee calls `thread_ptrace_stop(SIGTRAP)`
3. Tracee snapshots regs, sets `ptrace_stopped=1`, broadcasts `ptrace_cond`
4. Tracer's wait4 returns with `WIFSTOPPED(status)` and `WSTOPSIG==SIGTRAP`
5. Tracer reads/writes regs via GETREGSET/SETREGSET
6. Tracer calls PTRACE_CONT → sets `ptrace_stopped=0`, signals `resume_cond`
7. Tracee applies dirty regs, resumes execution

### wait4 Integration

`sys_wait4()` first checks for ptraced/vm-clone children via
`thread_ptrace_wait()` before falling through to the process table
for regular fork children. This handles both ptrace-stopped and
vm-exited states.
