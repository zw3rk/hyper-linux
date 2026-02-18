# hyper-linux

Run static aarch64-linux ELF binaries on macOS Apple Silicon via Hypervisor.framework.

## Architecture

- **hl.c** — Main entry point. CLI, VM setup, vCPU loop (~500 lines).
- **guest.c/h** — Guest memory management. Page tables, read/write, brk/mmap.
- **elf.c/h** — ELF64 parser and loader for static aarch64-linux binaries.
- **syscall.c/h** — Linux syscall dispatch and handlers (write, writev, mmap, brk, etc.).
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
| #5 | Syscall forward | X0-X5=args, X8=syscall nr |

## Build

```
make hl          # build + codesign hl
make test-hello  # build and run assembly hello world
make clean       # remove _build/
```

Requires macOS with Apple Silicon, Hypervisor.framework entitlement, and
nix develop shell with aarch64-unknown-linux-musl cross toolchain.

## Memory Layout

```
0x00010000 - 0x000FFFFF:  Page table pool (960KB)
0x00100000 - 0x001FFFFF:  Shim code (2MB block, RX)
0x00200000 - 0x003FFFFF:  Shim data/stack (2MB block, RW)
0x00400000 - varies:       ELF LOAD segments
0x01000000:                brk base (16MB)
0x07E00000 - 0x07FFFFFF:  Stack (2MB block, RW, grows down from 0x08000000)
0x10000000 - 0x1FFFFFFF:  mmap region (256MB, pre-mapped RW)
```

Total: 512MB mapped at IPA 0x0.
