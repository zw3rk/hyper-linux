# Rosetta Linux x86_64 Support — Research & Engineering Log

## Overview

hl supports x86_64-linux ELF binaries via Apple's Rosetta Linux translator
at `/Library/Apple/usr/libexec/oah/RosettaLinux/rosetta`. This document
captures everything learned during development: what works, what failed,
and the architectural decisions made.

## The Rosetta Binary

- **Path:** `/Library/Apple/usr/libexec/oah/RosettaLinux/rosetta`
- **Type:** ELF 64-bit ARM aarch64, ET_EXEC (not relocatable), statically linked
- **Load address:** 0x800000000000 (128 TB)
- **Size:** ~1.7 MB (3 LOAD segments: RO, RX, RW)
- **Entry:** 0x8000000261ac
- **Purpose:** JIT-translates x86_64 Linux syscalls → ARM64 Linux syscalls

### Companion: rosettad

- **Path:** `/Library/Apple/usr/libexec/oah/RosettaLinux/rosettad`
- **Type:** Same format (static aarch64-linux ELF)
- **Size:** ~384 KB
- **Commands:** `translate`, `daemon`, `digest`, `cfg`
- **Purpose:** AOT translation and caching daemon

## VZ Protocol — The Three Ioctls

Rosetta's entire VZ protocol is 3 ioctls during initialization. There is
**NO ongoing MMIO protocol** — VZ mode is purely initialization-time.

### 1. VZ_CHECK (0x80456125) = `_IOR('a', 0x25, 69)`
Returns a 69-byte signature that rosetta memcmp's:
```
"Our hard work\nby these words guarded\nplease don't steal\n\xc2\xa9 Apple Inc"
```
Failure → rosetta aborts with "Rosetta is only intended to run on Apple Silicon..."

### 2. VZ_CAPS (0x80806123) = `_IOR('a', 0x23, 128)`
Returns 128 bytes of host capabilities:
- `caps[0]`: Master VZ enable flag (non-zero = VZ features available)
- `caps[1]`: Sub-capability (read if caps[0]!=0 && caps[108]==0)
- `caps[108]`, `caps[109]`: Additional flags
- `caps[3..108]`: 106 bytes of config copied to internal state

**Key finding:** VZ_CAPS success does NOT change the JIT code path. Both
VZ and non-VZ modes use the same ExecutableHeap (0xf00000000000) and the
same forward-scan JIT engine. VZ mode enables AOT cache loading paths.

### 3. VZ_ACTIVATE (0x6124) = `_IO('a', 0x24)`
Handshake with hypervisor. Returns 0 on success. Gracefully handles
EOPNOTSUPP (95) and ENOTTY (25).

## What Works

### Simple x86_64 Binaries (Direct Calls Only)
- `test-write`: prints "OK" via direct write() syscall — **PASSES**
- `test-ret42`: returns 42 via direct exit() — **PASSES**

These work because all code is reachable via the JIT's forward scan from
the entry point. No indirect branches (function pointers, vtables) needed.

### Architecture
- 48-bit IPA verified working (Apple Silicon supports it despite reporting 36 default)
- Multi-region guest memory (rosetta at 128TB, binary at 0x400000)
- Rosetta mapped via `guest_add_mapping()` to separate host buffer
- All syscalls appear as ARM64 SVC #0 (rosetta handles x86_64→ARM64 mapping)

## What Fails

### Complex Binaries (Indirect Branches)
- `test-putchar`: calls musl's `putchar()` which uses `FILE*->write` function pointer — **FAILS**
- `test-puts`: same issue — **FAILS**
- All coreutils, busybox: likely fail (untested, same root cause)

**Root cause:** Rosetta's non-VZ JIT engine translates code by forward scan
from the entry point. Functions only reachable via indirect calls (function
pointers, computed jumps) never enter the translated block list. When an
indirect branch targets an untranslated address, rosetta asserts:
`"BasicBlock requested for unrecognized address"` (BuilderBase.h:550).

The assertion location: 0x800000038838, in `block_for_offset()`.

## Approaches Tried and Results

### Approach 1: Binary Patching (ABANDONED)

**Rationale:** Patch rosetta's internal assertions and code to handle
untranslated indirect branch targets.

**Patches implemented:**
1. **set_pointer assertion NOP** (TaggedPointer.h:33)
   - Address: 0x800000034938 (CBNZ x8 → NOP)
   - Purpose: Allow kernel VA pointers in tagged pointer system
   - Result: WORKED — assertion bypassed

2. **block_for_offset trampoline** (BuilderBase.h:550)
   - Address: 0x800000038838 (10 instructions)
   - Purpose: Create blocks on-demand for untranslated addresses
   - Result: Got past the first assertion, hit second assertion

3. **Binary search → linear search** (block_for_offset internals)
   - Address: 0x800000038aac (21 instructions)
   - Purpose: Handle unsorted block list after on-demand creation
   - Result: WORKED for its purpose but insufficient

**Outcome:** Each patched assertion revealed a deeper one. After
block_for_offset, the translator hit `_pending_internal_fixups.empty()`
(TranslatorBase.hpp:167). The JIT's internal fixup tracking fundamentally
requires sequential forward translation. On-demand block creation at
arbitrary offsets leaves unresolved fixups.

**Conclusion:** Patching individual assertions is a dead end. The non-VZ
JIT's forward-scan model is deeply integrated and cannot be extended to
handle arbitrary indirect branch targets via binary patches.

### Approach 2: Kernel VA via kbuf + TTBR1 (PARTIALLY ABANDONED)

**Rationale:** Rosetta mmaps internal data at kernel VA (0xFFFFFFFFF0000000+).
Emulate this via TTBR1 page tables backed by a 256MB host buffer.

**Implementation:**
- 256MB kbuf at GPA offset within primary buffer
- TTBR1 page tables (L0→L1→L2) mapping top 256MB of VA space
- User VA alias (KBUF_USER_VA = 0x0000FFFFF0000000) at same GPA

**Problem:** Rosetta stores pointers from kbuf allocations in its
TaggedPointer system, which asserts `(pointer_value & kValueMask) == 0`.
Kernel VA (0xFFFF...) has bits 63:48 set, violating this assertion.

**Fix discovered:** Return USER VA aliases from kernel VA mmap requests
instead of the kernel VA itself. The backing memory still lives in kbuf,
but rosetta sees user VA (0x0000FFFFF...) which passes the assertion.
Page tables map user VA → kbuf GPA via TTBR0.

**Current status:** kbuf is initialized for backing storage. sys_mmap
returns user VA aliases for kernel VA requests. In practice, rosetta's
high-VA allocations go through `sys_mmap_high_va` (the user VA allocator)
rather than the kbuf kernel VA path, so the kbuf path may not even be
needed anymore. Further investigation required.

### Approach 3: VZ Mode Activation (COMPLETE — Necessary but Insufficient)

**Rationale:** Return success for VZ_CAPS to enable rosetta's AOT code
paths (pre-translated binary loading, rosettad daemon integration).

**Status:** VZ_CAPS returns success with caps[0]=1. VZ_ACTIVATE returns 0.
Combined with user VA aliasing, this eliminates the TaggedPointer assertion.
Simple binaries (hello-write, hello-musl, echo-test) work. AOT translation
via rosettad protocol is fully functional — rosetta requests translation,
receives AOT file, and loads it (confirmed by pread64 + mmap on AOT fd).

**However,** AOT alone is insufficient. rosettad's static CFG analysis
produces incomplete translations (e.g., misses 0x402da0 in printf_core —
a rotated loop where the loop head is only reachable from its own backedge).
When the JIT encounters an address not in AOT's `_potential_targets`, it
asserts: `"BasicBlock requested for unrecognized address"`.

**Root cause:** AOT populates `_potential_targets` (which the assertion
checks: `_mode == TranslationMode::Aot || _potential_targets.empty()`).
With AOT loaded but incomplete, the JIT sees non-empty potential_targets
and asserts on missing addresses. This is a broken hybrid state.

**Next step:** Implement ptrace (Approach 5).

### Approach 4: Pre-map / Preannounce Segments (FAILED)

**Rationale:** Show x86_64 binary's segments in /proc/self/maps BEFORE
rosetta reads them, so the JIT knows which addresses contain code.

**Variants tried:**
- Pre-map all segments in regions[]
- Preannounce in separate array (appears in /proc/self/maps only)
- Preannounce exec-only segments
- Preannounce all + elf_map

**Result:** None helped. Rosetta reads /proc/self/maps once at startup to
build its module list, but the forward scan still starts from the entry
point and only translates sequentially. Code unreachable by forward scan
remains untranslated regardless of /proc/self/maps contents.

### Approach 5: Implement ptrace + clone(CLONE_VM) (COMPLETE — Not Used by Rosetta)

**Rationale:** Rosetta was believed to use a two-process ptrace-based JIT.

**Implementation:** Fully implemented ptrace (SEIZE, CONT, INTERRUPT,
GETREGSET, SETREGSET) and clone(CLONE_VM) in syscall_proc.c and fork_ipc.c.

**Result:** Rosetta NEVER calls clone() or ptrace() in traces. The ptrace
strings in the binary ("ptrace seize failed", "Expected inferior to be
stopped by SIGTRAP") are for the **GDB debug server** feature
(ROSETTA_DEBUGSERVER_PORT), NOT for the normal JIT execution path.

**Conclusion:** ptrace infrastructure is correct but irrelevant to the
core JIT failure. Rosetta's normal JIT uses in-process SIGTRAP signal
handling, not ptrace.

### Approach 6: SIGTRAP Signal Handler JIT Retranslation (CURRENT)

**Rationale:** When rosetta encounters an untranslated indirect branch
target, it hits a BRK stub → SIGTRAP → signal handler attempts
retranslation. In a real Lima VM, this works. In hl, the handler
immediately gives up and terminates.

**Observed behavior (from verbose trace):**
1. BRK #9 fires at 0xeffff8000010 (JIT cache)
2. hl delivers SIGTRAP to rosetta handler at 0x800000096490
3. Handler runs, makes NO translation syscalls
4. Handler calls rt_sigaction(SIGTRAP, SIG_DFL) — resetting handler
5. Handler calls rt_tgsigqueueinfo(1, 1, 5, siginfo) — re-raises SIGTRAP
6. Handler calls rt_sigreturn
7. Re-raised SIGTRAP fires with SIG_DFL → terminate (exit 133)

**The handler gives up immediately without attempting retranslation.**
Something in the handler's initial data structure lookup fails.

## AOT Translation Architecture

### Two Cache Systems

| Aspect | `.flu` files | `.aotcache` files |
|--------|-------------|-------------------|
| Created by | rosetta runtime | rosettad daemon |
| Location | `$HOME/.cache/rosetta/` | `<daemon_cache_path>/` |
| Naming | `<sha256>.flu` | `<sha256>.aotcache` |
| Purpose | JIT-promoted runtime cache | Daemon pre-computed AOT |
| Requires daemon | No | Yes |

### rosettad Commands

```
rosettad translate <input> <output>   # One-shot AOT translation
rosettad daemon [<cache_path>]        # Run daemon (default: $HOME/.cache/rosettad)
rosettad digest <file>                # Compute SHA-256 digest
rosettad cfg <binary>                 # Build control-flow graph
```

### Environment Variables

| Variable | Effect |
|----------|--------|
| `DISABLE_AOT` | Skip AOT cache loading |
| `AOT_ERRORS_ARE_FATAL` | Fatal on AOT load failure (default: fallback to JIT) |
| `ROSETTA_DEBUGSERVER_PORT` | Wait for debugger |
| `DISABLE_EXCEPTIONS` | Disable exception handling |
| `DISABLE_SIGACTION` | Disable signal action handling |
| `ALLOW_GUARD_PAGES` | Allow guard page protection |

### Daemon Socket

rosettad listens on: `<cache_path>/uds/rosetta.sock`
Two mechanisms for VZ guest access:
1. Unix Domain Socket (UDS) via file sharing
2. Abstract socket (macOS 14+)

### rosettad Protocol (hl implementation)

hl intercepts rosetta's SOCK_SEQPACKET socket creation and uses a
SOCK_DGRAM socketpair (macOS lacks SEQPACKET). A handler thread processes:

| Command | Description | Response |
|---------|-------------|----------|
| `'?'` | Handshake | `0x01` (ready) |
| `'t'` | Translate (binary fd via SCM_RIGHTS) | 3 separate messages: `0x01` + 32-byte digest + AOT fd (SCM_RIGHTS, 1-byte iov) |
| `'d'` | Digest cache lookup (32-byte SHA256) | `0x00` (not cached) |
| `'q'` | Quit | (close) |

**Critical protocol details:**
- Translate response MUST be 3 separate writes (atomic sendmsg hangs rosetta)
- AOT fd send MUST use exactly 1 byte iov payload (not 8) — rosetta's recv_fd
  allocates 1-byte buffer; extra bytes trigger MSG_TRUNC on SOCK_DGRAM
- VZ_CAPS caps[3..108] provides daemon socket path; currently zeros (intercepted
  anyway via socketpair)

## TranslationMode Architecture (Reverse-Engineered)

Rosetta uses three independent mode/quality concepts that interact but
must not be confused:

### 1. Translation Quality (BSS[0x474])

Hardware-dependent, set once during initialization at VA 0x800000030c64:
- **1 = basic**: M2 (Apple A15 derivative), no FEAT_AFP support
- **2 = enhanced**: M3+ (Apple A17+), has FEAT_AFP

Detection: reads `ID_AA64MMFR1_EL1`, checks AFP field (bits 44-47).
This affects code generation quality, not the JIT vs AOT decision.

### 2. TranslationMode Enum (Per-Translation-Unit)

Stored at object offset 0x18, read in the translate dispatch at
VA 0x800000039e00 (`ldrb w8, [x19, #0x18]`):

| Value | Name | Description |
|-------|------|-------------|
| 0 | Aot | Fully pre-translated AOT cache (TranslationCacheAot.cpp) |
| 1 | JIT | Pure JIT — forward scan from entry, no AOT data |
| 2 | AOT-assisted JIT | JIT with AOT-seeded blocks |
| 3 | (unknown) | Separate handler at 0x3a2e8 |

**Critical finding: The JIT translate path (big function at 0x46cb0)
NEVER uses TranslationMode::Aot (0).** The mode computation at
VA 0x800000034f78 produces only mode 1 or 2:

```asm
; [sp, #0x48] = is_aot_mode (from per-fragment flag at object+0x214)
and w8, w8, #0xff    ; mask to byte
cmp w8, #1           ; compare
mov w8, #1           ; default = JIT (1)
cinc w8, w8, eq      ; if is_aot_mode == 1: mode = 2 (AOT-assisted)
sturb w8, [x29, #-0x44]  ; store as local _mode
```

### 3. is_aot_mode Flag (Per-Fragment)

Stored at object offset 0x214. Extracted at VA 0x80000004901c:
```asm
; x8 loaded from [x21, #0x88] (AOT metadata flags in fragment header)
ubfx w8, w8, #8, #1    ; extract bit 8
strb w8, [x20, #0x214] ; store as is_aot_mode
```

When this flag is set (1), the translate caller at 0x33688 forces
`w2=1` and enters the big translate with mode=2 (AOT-assisted JIT).

### TranslationMode::Aot (0) — Separate Code Path

Mode 0 is used by TranslationCacheAot.cpp for fully pre-translated
AOT cache entries. It has its own dispatch path at 0x39e1c which calls
function 0x39378 (AOT translation cache operations). Features exclusive
to mode 0:
- `translate_indirect_jmp_dyld_stub` (asserts `_mode == Aot` at 0x39ed0)
- `_potential_targets` usage (the tree at [sp,#0x150] is only read
  in the mode==0 path at 0x47138-0x47144)
- Set by a lookup function at 0x29ae4 that searches a table at
  0x80000001c380

Mode 0 is set when a fully translated AOT cache file (`.flu` or
`.aotcache`) exists and the translation unit lookup succeeds.

### How AOT-Assisted JIT (Mode 2) Works

When rosettad provides an AOT translation:

1. **Fragment loading:** The AOT file's fragment header at offset 0x88
   has bit 8 set, causing `is_aot_mode=1` (object+0x214)

2. **Mode selection:** The translate caller sees `is_aot_mode=1` and
   passes `w2=1`, which the big translate function converts to mode=2

3. **Block seeding (0x46ee8):** Mode 2 calls `seed_block()` at
   VA 0x80000002a7b0 with `w1=0` (entry offset). This function:
   - Checks a bitmap at object+0x18 for already-registered blocks
   - If new, increments block count at object+0x28
   - Performs binary search (0x2a914) to find insertion point
   - Adds block offset to sorted array at object+0x30

4. **Main translate loop:** Both modes enter the same loop at 0x475a4.
   Inside the loop at 0x47cdc:
   - Mode 2 takes a short path (`b.eq 0x47f8c`) that skips the
     block list iteration (0x47ce8-0x47d3c)
   - Mode 1 performs the full block list scan

5. **block_for_offset (0x387d4):** Uses binary search on the block
   table. With AOT-seeded blocks, lookups succeed for all known
   addresses. Returns NULL (assertion) only for addresses not in
   any pre-registered block.

### The `_potential_targets` Assertion

At VA 0x800000048eac (`_mode == TranslationMode::Aot || _potential_targets.empty()`,
BranchTargetFinderBase.hpp:57):

```asm
800000047444: ldrb w10, [sp, #0x110]   ; load _mode
800000047448: cbz  w10, 0x47454        ; if mode==0 (Aot): skip check
80000004744c: ldr  x10, [sp, #0x158]   ; load _potential_targets.end
800000047450: cbnz x10, 0x48eac        ; if non-empty: ASSERT
```

Within the big translate function, `_potential_targets` at [sp,#0x150]:
- Initialized to zero at 0x46e54: `stp xzr, xzr, [sp, #0x150]`
- Reset to zero at 0x4758c: `stp xzr, xzr, [sp, #0x150]`
- **Never populated with non-zero values in this function**

The assertion is a defensive invariant: potential targets should only
exist in Aot mode (0). In JIT mode (1) and AOT-assisted mode (2),
they must be empty. Since no code path in the big translate function
populates `_potential_targets`, this assertion should never fire here.
It would only fire if a called function corrupted the stack.

### BSS[0xa04] — VZ Enable Flag (rosettad Gate)

Written at VA 0x800000030304 from VZ_CAPS caps[0]:
```asm
8000000301f4: adrp  x19, 0x8000000a0000
8000000301f8: add   x19, x19, #0xa04    ; x19 = &BSS[0xa04]
...
800000030304: strb  w20, [x19]           ; BSS[0xa04] = caps[0]
800000030308: strb  w21, [x19, #0x1]    ; BSS[0xa05] = caps[108]
80000003030c: strb  w22, [x19, #0x2]    ; BSS[0xa06] = caps[1] or derived
```

The runtime AOT loading path at 0x90ae8 checks both flags:
```asm
ldrb w8, [x8, #0x498]    ; DISABLE_AOT env flag
cbnz w8, skip_aot         ; if set, no AOT
ldrb w8, [x8, #0xa04]    ; VZ enable flag (from caps[0])
cbz  w8, jit_only          ; if zero, JIT-only mode
; ... proceed to rosettad connection
```

When hl sets caps[0]=1 in VZ_CAPS response, BSS[0xa04]=1, enabling
the rosettad connection path.

### BSS Layout Summary (0x8000000a0000+)

| Offset | Size | Description |
|--------|------|-------------|
| 0x474 | 1 | Translation quality (1=basic, 2=enhanced) |
| 0x496 | 2 | Flag checked after VZ_ACTIVATE |
| 0x498 | 1 | DISABLE_AOT env flag |
| 0x49b | 1 | Set during init loop |
| 0x49c | 1 | Checked after translate call |
| 0x49f | 1 | Rosetta version/signature byte |
| 0xa04 | 1 | VZ enable flag (= VZ_CAPS caps[0]) |
| 0xa05 | 1 | VZ sub-capability (= VZ_CAPS caps[108]) |
| 0xa06 | 108 | Rosettad socket path (= VZ_CAPS caps[1..108]) |

### Answer to Key Questions

**Q: Is TranslationMode::Aot set when the ENTIRE binary is AOT-translated?**
A: Yes. Mode 0 (Aot) is used for fully pre-translated cache entries via
TranslationCacheAot.cpp. It is a per-translation-unit setting, not global.
Each translation unit can independently be mode 0 (fully cached), mode 1
(JIT), or mode 2 (AOT-assisted JIT).

**Q: On M2 (no FEAT_AFP, quality=1), when rosettad provides AOT, does
rosetta set TranslationMode::Aot?**
A: No. When rosettad provides AOT, rosetta sets `is_aot_mode=1` (per-fragment
flag at object+0x214), which causes mode=2 (AOT-assisted JIT), NOT mode=0
(Aot). Mode 0 is only for fully pre-translated cache entries that go through
a completely different code path (TranslationCacheAot.cpp).

**Q: What prevents the block_for_offset assertion from firing in a real VM?**
A: In a real VZ VM (Tart/Lima), rosetta uses the ptrace two-process
architecture:
1. `clone(CLONE_VM)` creates an inferior sharing address space
2. `PTRACE_SEIZE` attaches the tracer
3. BRK instructions at untranslated addresses trigger SIGTRAP
4. Tracer catches via wait4(), reads regs, translates the block on-demand
5. Tracer writes patched code, calls PTRACE_CONT

This on-demand translation via ptrace-stop/translate/continue means
blocks are created as needed when reached, so `block_for_offset()` always
finds the block. The forward-scan limitation only applies to the non-ptrace
fallback path.

AOT-assisted mode (2) helps by pre-seeding known blocks, reducing ptrace
stops. But the ptrace architecture is the fundamental mechanism that handles
ALL indirect branch targets — including those that static CFG analysis
misses (rotated loops, computed gotos, function pointers).

## Key Technical Details

### HVF 48-bit IPA
Apple Silicon supports 48-bit IPA (256 TB) despite `hv_vm_config_get_max_ipa_size()`
reporting 36 bits as default. Verified: IPA=36,40,44,48 all work.

### Host mmap Limitation
macOS user address space tops out at 2^47 (128 TB). `mmap(0x800000000000, ...)`
fails. Solution: allocate host buffer at low address, map via `hv_vm_map()`
to high IPA, use guest page tables for VA→IPA translation.

### Syscall Transparency
Rosetta translates x86_64 `syscall` → ARM64 `SVC #0` with ARM64 syscall
numbers. hl's existing ~140 ARM64 syscall handlers work transparently.
No x86_64 syscall table needed.

### W^X Page Permission Toggling
Rosetta's JIT needs RW (write code) then RX (execute code). HVF enforces
W^X. Solution: 4KB page granularity permission toggling via HVC #9 from
the shim. Splits 2MB blocks into 512 × 4KB L3 pages on first toggle.

### Tagged Pointer System
Rosetta uses a tagged pointer system (TaggedPointer.h) that stores metadata
in bits 63:48 of pointers. It asserts that input pointer values have
bits 63:48 == 0 before overwriting them with the tag via BFI. Kernel VA
(0xFFFF...) violates this assertion. Solution: return user VA aliases from
kernel VA mmap requests.

## External References

- [Apple: Running Intel Binaries in Linux VMs with Rosetta](https://developer.apple.com/documentation/virtualization/running-intel-binaries-in-linux-vms-with-rosetta)
- [Project Champollion: Rosetta AOT analysis](https://ffri.github.io/ProjectChampollion/part1/)
- [Lima: Rosetta AOT caching](https://github.com/lima-vm/lima/pull/2489)
- [rosetta-multipass: FUSE mount for Rosetta](https://github.com/mrexodia/rosetta-multipass)
- [Quick look at Rosetta on Linux](https://threedots.ovh/blog/2022/06/quick-look-at-rosetta-on-linux/)
- [Why is Rosetta 2 fast?](https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/)
