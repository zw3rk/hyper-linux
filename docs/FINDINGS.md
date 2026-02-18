# Consolidated Research Findings

Findings from 6 exploratory projects investigating macOS Hypervisor.framework
on Apple Silicon (aarch64-darwin). Projects span June-July 2025.

Sources: `hypervisor-els`, `hypervisor-setup`, `hypervisor-demo`,
`hyper-linux` (Zig), `hyper-linux-shim-analysis`, `hypervisor-grok`.

---

## 1. Exception Level Architecture

### Security Model

```
Host process (EL0) → Hypervisor.framework → Real EL2 → Guest EL1 → Guest EL0
```

The host application runs at macOS EL0. Hypervisor.framework talks to the real
EL2 hypervisor. Guest code starts at EL1 and can transition to EL0 via ERET.

### HCR_EL2 Configuration (Inferred)

Cannot read HCR_EL2 from guest. Inferred from testing 16,384+ register
encodings:

```
HCR_EL2.TSC = 1   // Trap all MSR/MRS to system registers
HCR_EL2.TWI = 1   // Trap WFI
HCR_EL2.TWE = 1   // Trap WFE
HCR_EL2.HCD = 0   // HVC not disabled (used for guest→host communication)
HCR_EL2.TGE = 0   // Don't trap general exceptions to EL2
```

### Asymmetric Register Trapping (Major Discovery)

Testing 16,384 system register encodings revealed:
- **16,388 registers can be READ without trapping** (performance optimization)
- **ALL registers trap on WRITE** (security enforcement)
- Read-only without trap: CPU ID regs, timers, thread pointers, CurrentEL,
  CTR_EL0, DCZID_EL0
- All writes redirect to PC=0x200

### MSR Trapping — The Root Cause of Many Issues

All MSR instructions to system registers are trapped by HCR_EL2.TSC=1. This
was initially misdiagnosed as "ERET trapping" — it's actually the MSR
instructions that set up SPSR_EL1/ELR_EL1 before ERET that trap.

Trap addresses:
- **PC=0x200**: MSR/MRS instruction trap (most common)
- **PC=0x400**: Illegal instruction at EL0
- **PC=0x0**: WFI, WFE, SMC, some HVC

**Workaround**: HVC-based register service. Guest requests host to set system
registers via HVC #4. Host uses `hv_vcpu_set_sys_reg()` API.

### ERET from EL1 to EL0

ERET works when system registers are configured from the host side:

```c
hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, 0);       // EL0t
hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, target_pc); // Target
hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, stack);      // Stack
// Guest executes ERET — it works
```

ERET itself is NOT trapped. Only the MSR instructions that prepare for it are.

---

## 2. MMU Configuration

### Mandatory MMU

Any load/store instruction causes a translation fault (EC=0x20) without the
MMU enabled. The MMU is not optional — it must be configured before any memory
access in the guest.

### Working Configuration

Verified working configuration for guest MMU:

| Register | Value | Meaning |
|----------|-------|---------|
| SCTLR_EL1 | M=1, A=0, C=1, I=1 | MMU on, alignment check off, caches on |
| MAIR_EL1 | 0xFF00 | Attr0=device (0x00), Attr1=normal WB (0xFF) |
| TCR_EL1 | 0x5B5103510 | 4KB granule, 48-bit VA, inner shareable |
| TTBR0_EL1 | page table PA | L0 table base |
| CPACR_EL1 | 3 << 20 | Enable FP/SIMD at EL0/EL1 |

### 2MB Block Mappings — Critical for Misaligned Access

**This is the single most important finding.** Guest page tables MUST use 2MB
block mappings (L2 block descriptors with 4KB granule) for transparent
misaligned memory access.

Working block sizes:
- **2MB blocks** (L2 with 4KB granule) — VERIFIED working
- 32MB blocks (L2 with 16KB granule) — expected to work (architectural)
- 512MB blocks (L2 with 64KB granule) — expected to work (architectural)

Non-working page sizes:
- **4KB pages** — VERIFIED to hang on misaligned access
- 16KB pages — expected to hang
- 64KB pages — expected to hang

The block vs page distinction is in the **guest's** page tables, not the
hypervisor's memory mapping. A guest can mix mappings: use blocks for regions
needing misaligned access and pages elsewhere. This is compatible with Linux's
transparent huge pages (2MB).

### Page Table Layout (Current Implementation)

```
L0[0] → L1 table (at GPA+0x2000)
  L1[0]: 1GB device block (0x0-0x3FFFFFFF)      — Attr0, UXN, PXN
  L1[1]: 1GB normal block (0x40000000-0x7FFFFFFF) — Attr1, RO
  L1[2] → L2 table (at GPA+0x3000)
    L2[0]: 2MB code block (0x80000000-0x801FFFFF) — RX, Attr1
    L2[1]: 2MB data block (0x80200000-0x803FFFFF) — RW, XN, Attr1
```

### ADRP and Block Mappings

ADRP's "page" is always 4KB (architectural), independent of MMU block size.
No conflict with 2MB blocks. The linker should align sections to avoid
crossing 2MB block boundaries.

### Virtual Address Space

- 39-bit VA (T0SZ=25): Tested, verified working. 3-level page tables.
- 48-bit VA (T0SZ=16): Hypervisor accepts it. 4-level page tables. Currently
  used in shim.S.

---

## 3. CPACR_EL1 and SIMD

Default CPACR_EL1 = 0x555, which sets FPEN (bits 21:20) = 00, trapping all
FP/SIMD instructions. This was initially misdiagnosed as an alignment problem.

**Fix**: Set CPACR_EL1 FPEN = 0b11 (bits 21:20) via HVC #4 to enable FP/SIMD
at both EL0 and EL1. The hypervisor allows guests to control their own FP/SIMD
access.

---

## 4. Shim Architecture

The shim runs at EL1 and acts as a minimal kernel between the hypervisor and
EL0 user code.

### Responsibilities

1. Configure system registers (via HVC #4 to host)
2. Set up page tables and enable MMU
3. Install exception vectors (VBAR_EL1)
4. Transition to EL0 (ERET)
5. Handle EL0 exceptions:
   - SVC: dispatch by immediate (SVC #1 = increment x0, SVC #2 = debug print)
   - Alignment faults: forward to host or emulate
   - Other: report via HVC #2

### Register Setup Sequence

The shim uses HVC #4 to set registers in a specific order:
1. VBAR_EL1 → exception vectors
2. MAIR_EL1 → memory attributes
3. TCR_EL1 → translation control
4. TTBR0_EL1, TTBR1_EL1 → page table bases
5. TLBI + DSB + ISB → flush TLB
6. SCTLR_EL1 → enable MMU (M=1, I=1)
7. CPACR_EL1 → enable FP/SIMD
8. ELR_EL1 → user code entry point
9. SPSR_EL1 → EL0 AArch64 (0x0)
10. SP_EL0 → user stack
11. ERET

### Exception Vector Layout

Standard ARM64 vector table (2KB aligned):
- 0x000-0x180: Current EL, SP0 (unused, → bad_exception)
- 0x200: Current EL, SPx, Synchronous → svc_handler
- 0x280-0x380: Current EL, SPx, IRQ/FIQ/SError (unused)
- 0x400: Lower EL, A64, Synchronous → svc_handler
- 0x480-0x580: Lower EL, A64, IRQ/FIQ/SError (unused)
- 0x600-0x780: Lower EL, A32 (unused)

---

## 5. Memory Layout

```
GPA 0x80000000  +0x0000: shim.bin (EL1 kernel)
                +0x0800: exception vectors (2KB aligned)
                +0x1000: L0 page table
                +0x2000: L1 page table
                +0x3000: L2 page table
                +0x4000: user.bin (EL0 code)
                ...
GPA 0x80200000: Writable data page (2MB, XN)
                Stack grows down from 0x80400000
GPA 0x81000000: End of guest memory (16MB total)
```

Host maps entire 16MB as RWX via `hv_vm_map()`. Guest page tables enforce
finer-grained permissions (code RX, data RW+XN).

---

## 6. Common Pitfalls

1. **Over-configuring VBAR_EL1**: Setting custom exception vectors can conflict
   with framework defaults. Only set after MMU is properly configured.

2. **MSR before HVC workaround**: Any attempt to use MSR directly will trap.
   Always use the HVC #4 protocol for system register writes.

3. **4KB pages for code regions**: Will hang on misaligned access. Use 2MB
   blocks, even for small code.

4. **Forgetting CPACR_EL1**: Default traps all FP/SIMD. Must explicitly enable.

5. **PSTATE/CPSR assumption**: Always explicitly set CPSR to 0x3c5 (EL1h)
   even if it reads correctly.

6. **Cache/TLB coherency**: Always TLBI + DSB + ISB before enabling MMU and
   after changing page tables.

7. **Host memory alignment**: Host memory for `hv_vm_map` should be page-aligned
   (use `posix_memalign`).

8. **HVC from EL0 is UNDEFINED**: EL0 code cannot execute HVC. Use SVC to trap
   to the EL1 shim, which can then HVC to the host.

---

## 7. Working Patterns

### Guest → Host Communication

```
EL0: SVC #N        →  EL1 shim exception vector
EL1: HVC #N        →  Host HV_EXIT_REASON_EXCEPTION (EC=0x16)
Host: read/modify registers, advance PC or not, resume vcpu
```

### System Register Write (from guest perspective)

```asm
mov x0, #<reg_id>   // Register ID (0=VBAR, 1=MAIR, etc.)
mov x1, <value>      // Value to write
hvc #4               // Host writes the register
isb                   // Barrier after register change
```

### Debug Output (from EL0)

```asm
mov x0, #<value>
svc #2               // Shim forwards to host via HVC #1
```

---

## 8. Project History

| Project | Date | Focus | Key Outcome |
|---------|------|-------|-------------|
| hypervisor-els | Jul 2025 | Exception levels, MSR trapping | Discovered TSC trapping, HVC workaround |
| hypervisor-setup | Jul 2025 | MMU, alignment, page tables | Proved 2MB blocks needed |
| hypervisor-demo | Jul 2025 | Block mapping proof of concept | Verified 2MB blocks work |
| hyper-linux (Zig) | Jun-Jul 2025 | Full Linux VM attempt | Zig + shim architecture |
| hyper-linux-shim-analysis | Jul 2025 | Shim variant analysis | 30+ shim variants catalogued |
| hypervisor-grok | Jul 2025 | Clean C rewrite | **Current active code** |
