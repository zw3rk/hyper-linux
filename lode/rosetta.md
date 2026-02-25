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

### Approach 3: VZ Mode Activation (CURRENT)

**Rationale:** Return success for VZ_CAPS to enable rosetta's AOT code
paths (pre-translated binary loading, rosettad daemon integration).

**Status:** VZ_CAPS returns success with caps[0]=1. VZ_ACTIVATE returns 0.
Combined with user VA aliasing, this eliminates the TaggedPointer assertion.
test-write passes. But the JIT forward-scan limitation persists — VZ mode
alone doesn't fix indirect branches.

**Next step:** Integrate rosettad for AOT pre-translation.

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
