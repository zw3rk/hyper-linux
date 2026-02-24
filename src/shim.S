/* shim.S — EL1 kernel shim for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loaded at SHIM_BASE (0x100000). Runs at EL1.
 * All system registers (VBAR, MAIR, TCR, TTBR0, SCTLR, etc.) are
 * configured by the host before vCPU start. The shim entry point just
 * transitions to EL0 via ERET. Exception vectors handle SVC #0
 * (Linux syscall) forwarding to the host via HVC #5.
 *
 * HVC protocol:
 *   #0  Normal exit       (x0 = exit code)
 *   #2  Bad exception     (x0=ESR, x1=FAR, x2=ELR, x3=SPSR, x5=vector)
 *   #4  Set sysreg        (x0 = reg ID, x1 = value; host writes via HVF)
 *   #5  Syscall forward   (registers preserved from EL0 SVC #0)
 *
 * IMPORTANT: macOS `as` uses ';' as a comment character on AArch64,
 * NOT as a statement separator. Each instruction must be on its own line.
 *
 * IMPORTANT: The Linux syscall ABI requires ALL general-purpose registers
 * (X1-X30) to be preserved across SVC #0, with only X0 modified (return
 * value). Musl and other libc implementations rely on this — the compiler
 * does NOT save scratch registers (X9-X15) across SVC calls. Therefore,
 * the exception handler MUST save and restore ALL 31 GPRs.
 */

.text
.align 4
.globl _start
_start:
    /* Host has configured all system registers EXCEPT SCTLR.M (MMU enable).
     * Apple's Hypervisor.framework requires the MMU to be enabled DURING
     * vCPU execution (via HVC #4), not before hv_vcpu_run(). Setting
     * SCTLR.M=1 via hv_vcpu_set_sys_reg before start causes permission
     * faults on the first instruction fetch.
     *
     * Host passes the final SCTLR value (with M=1) in X0 before start. */

    /* Save SCTLR value from host (passed in X0) */
    mov x9, x0

    /* TLB invalidation — ensure clean TLB state before enabling MMU */
    tlbi vmalle1is
    dsb ish
    isb

    /* Enable MMU: ask host to write SCTLR (reg ID 4 = SCTLR_EL1) */
    mov x0, #4
    mov x1, x9
    hvc #4
    isb

    /* Zero all general-purpose registers before entering EL0 */
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, #0
    mov x5, #0
    mov x6, #0
    mov x7, #0
    mov x8, #0
    mov x9, #0
    mov x10, #0
    mov x11, #0
    mov x12, #0
    mov x13, #0
    mov x14, #0
    mov x15, #0
    mov x16, #0
    mov x17, #0
    mov x18, #0
    mov x19, #0
    mov x20, #0
    mov x21, #0
    mov x22, #0
    mov x23, #0
    mov x24, #0
    mov x25, #0
    mov x26, #0
    mov x27, #0
    mov x28, #0
    mov x29, #0
    mov x30, #0

    eret

/* ================================================================
 * Exception Vector Table
 * Must be 2KB (0x800) aligned. Each entry is 128 bytes (0x80).
 *
 * bad_exception vectors: mov x5, #offset + b bad_exception
 *   X5 carries the vector offset for host-side debugging.
 *   This is safe because bad_exception halts — no register
 *   preservation needed.
 *
 * svc_handler vectors: b svc_handler (NO mov x5!)
 *   These MUST NOT clobber any GPR before svc_handler saves
 *   them. The Linux syscall ABI requires X0-X30 to be preserved
 *   across SVC #0 (only X0 is modified with the return value).
 *   If we wrote to X5 here, the saved value would be wrong and
 *   the restored X5 after ERET would corrupt the caller's state.
 * ================================================================ */
.align 11
vectors:
    /* ---- Current EL, SP0 ---- */
    /* 0x000: Synchronous */
    mov x5, #0x000
    b bad_exception
    .align 7
    /* 0x080: IRQ */
    mov x5, #0x080
    b bad_exception
    .align 7
    /* 0x100: FIQ */
    mov x5, #0x100
    b bad_exception
    .align 7
    /* 0x180: SError */
    mov x5, #0x180
    b bad_exception

    /* ---- Current EL, SPx ---- */
    .align 7
    /* 0x200: Synchronous (from EL1) — must not clobber GPRs */
    b svc_handler
    .align 7
    /* 0x280: IRQ */
    mov x5, #0x280
    b bad_exception
    .align 7
    /* 0x300: FIQ */
    mov x5, #0x300
    b bad_exception
    .align 7
    /* 0x380: SError */
    mov x5, #0x380
    b bad_exception

    /* ---- Lower EL, AArch64 ---- */
    .align 7
    /* 0x400: Synchronous (from EL0) — must not clobber GPRs */
    b svc_handler
    .align 7
    /* 0x480: IRQ */
    mov x5, #0x480
    b bad_exception
    .align 7
    /* 0x500: FIQ */
    mov x5, #0x500
    b bad_exception
    .align 7
    /* 0x580: SError */
    mov x5, #0x580
    b bad_exception

    /* ---- Lower EL, AArch32 ---- */
    .align 7
    /* 0x600: Synchronous */
    mov x5, #0x600
    b bad_exception
    .align 7
    /* 0x680: IRQ */
    mov x5, #0x680
    b bad_exception
    .align 7
    /* 0x700: FIQ */
    mov x5, #0x700
    b bad_exception
    .align 7
    /* 0x780: SError */
    mov x5, #0x780
    b bad_exception

/* ================================================================
 * bad_exception — Report unexpected exception to host and halt
 * ================================================================ */
bad_exception:
    mrs x0, esr_el1
    mrs x1, far_el1
    mrs x2, elr_el1
    mrs x3, spsr_el1
    /* x5 already holds vector offset from the entry stub */
    hvc #2

/* ================================================================
 * svc_handler — Synchronous exception dispatcher
 *
 * On entry from EL0 SVC #0 (or EL1 synchronous):
 *   X0-X5  = syscall arguments
 *   X8     = syscall number
 *   X6-X30 = caller state (MUST be preserved per Linux ABI)
 *
 * The Linux syscall ABI guarantees all registers except X0 survive
 * the SVC. Musl's inline syscall wrappers (and GCC/Clang codegen)
 * rely on this: scratch registers X9-X15 are NOT saved by the
 * compiler across SVC calls. We must save/restore ALL 31 GPRs.
 *
 * Stack frame: 256 bytes (16 pairs × 16 bytes)
 *   [sp+0]   x0,x1    [sp+16]  x2,x3    [sp+32]  x4,x5
 *   [sp+48]  x6,x7    [sp+64]  x8,x9    [sp+80]  x10,x11
 *   [sp+96]  x12,x13  [sp+112] x14,x15  [sp+128] x16,x17
 *   [sp+144] x18,x19  [sp+160] x20,x21  [sp+176] x22,x23
 *   [sp+192] x24,x25  [sp+208] x26,x27  [sp+224] x28,x29
 *   [sp+240] x30,(pad)
 * ================================================================ */
svc_handler:
    /* Save all 31 general-purpose registers on the EL1 stack */
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]

    /* Decode exception class from ESR_EL1.
     * X9-X11 are now scratch (saved on stack above). */
    mrs x9, esr_el1
    lsr x10, x9, #26         /* EC = bits [31:26] */

    cmp x10, #0x15            /* SVC from AArch64? */
    b.ne not_svc

    /* Extract SVC immediate (bits [15:0]) */
    and x11, x9, #0xFFFF

    cmp x11, #0               /* SVC #0 = Linux syscall? */
    b.eq handle_svc_0

    /* Unrecognized SVC — restore and report as bad exception */
    b restore_and_bad

not_svc:
    /* Not an SVC — restore and report as bad exception */

restore_and_bad:
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256

    /* Determine correct vector offset for bad_exception reporting.
     * After restore, X5 holds the EL0 caller's value — not the vector.
     * Check SPSR_EL1.M[3:0]: 0=EL0t (Lower EL → 0x400), 4/5=EL1 (→ 0x200). */
    mrs x5, spsr_el1
    and x5, x5, #0xF
    cmp x5, #0
    mov x5, #0x400           /* Assume Lower EL (EL0 → vec 0x400) */
    b.eq bad_exception
    mov x5, #0x200           /* Current EL (EL1 → vec 0x200) */
    b bad_exception

/* ================================================================
 * handle_svc_0 — Linux syscall forwarding
 *
 * Restore ALL EL0 registers from the stack frame before HVC #5,
 * so the host can read X8 (syscall nr) and X0-X5 (args) directly
 * from vCPU registers. The host writes the result to X0 and
 * resumes the vCPU. We then ERET to EL0 with X0=result and all
 * other registers (X1-X30) in their original EL0 state.
 * ================================================================ */
handle_svc_0:
    /* Restore all 31 GPRs to their EL0 values */
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256

    /* Forward to host. Host will:
     *   1. Read X8 (syscall nr) and X0-X5 (args)
     *   2. Execute the syscall
     *   3. Write result to X0
     *   4. Set X8 to non-zero if page tables were modified (TLBI needed)
     *   5. Resume vCPU (we continue below) */
    hvc #5

    /* If the host modified page tables (X8 != 0), flush the TLB.
     * X8 is the syscall number register — the Linux ABI marks it
     * clobbered across SVC, so callers never expect it preserved. */
    cbz x8, 1f
    tlbi vmalle1is
    dsb ish
    isb
1:
    /* Return to EL0 — X0 now holds the syscall return value,
     * X1-X30 retain their original EL0 values. */
    eret
