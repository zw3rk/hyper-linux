/* hv_util.h — Shared Hypervisor.framework utility macros and constants
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provides HV_CHECK macro and SCTLR_EL1 constants used across hl.c,
 * fork_ipc.c, syscall_proc.c, and syscall_exec.c. Centralizes these
 * definitions to avoid duplication.
 */
#ifndef HV_UTIL_H
#define HV_UTIL_H

#include <Hypervisor/Hypervisor.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------- HV_CHECK macro ----------
 * Abort on HVF API failure. Used for calls that should never fail
 * during normal operation (vCPU register access, VM mapping). */
#define HV_CHECK(call) do {                                        \
    hv_return_t _r = (call);                                       \
    if (_r != HV_SUCCESS) {                                        \
        fprintf(stderr, "hl: %s failed: %d\n", #call, (int)_r);   \
        exit(1);                                                   \
    }                                                              \
} while (0)

/* HV_CHECK variant that emits a structured crash report before exit.
 * Use in the vCPU run loop where vcpu and guest_t are available. */
#define HV_CHECK_CTX(call, vcpu, g) do {                           \
    hv_return_t _r = (call);                                       \
    if (_r != HV_SUCCESS) {                                        \
        fprintf(stderr, "hl: %s failed: %d\n", #call, (int)_r);   \
        crash_report((vcpu), (g), CRASH_HV_CHECK, #call);         \
        exit(1);                                                   \
    }                                                              \
} while (0)

/* ---------- SCTLR_EL1 bits ----------
 * These are needed by hl.c (initial setup), fork_ipc.c (child MMU
 * enable), and syscall_exec.c (exec MMU re-enable). */
#define SCTLR_M   (1ULL << 0)   /* MMU enable */
#define SCTLR_C   (1ULL << 2)   /* Data cache enable */
#define SCTLR_I   (1ULL << 12)  /* Instruction cache enable */
#define SCTLR_UCT (1ULL << 15)  /* EL0 access to CTR_EL0 (cache type) */
#define SCTLR_UCI (1ULL << 26)  /* EL0 cache maintenance (IC IVAU, DC CVA*) */

/* RES1 bits in SCTLR_EL1 — these MUST be 1 for correct behaviour.
 * Apple Hypervisor.framework returns default SCTLR=0x0, so we must set
 * them explicitly. The hardware eventually auto-updates them, but the
 * initial instruction fetches execute with whatever we wrote. */
#define SCTLR_RES1 ((1ULL << 29) /* LSMAOE */  | \
                     (1ULL << 28) /* nTLSMD */  | \
                     (1ULL << 23) /* SPAN   */  | \
                     (1ULL << 22) /* EIS    */  | \
                     (1ULL << 20) /* TSCXT  */  | \
                     (1ULL << 11) /* EOS    */  | \
                     (1ULL <<  8) /* SED    */  | \
                     (1ULL <<  7) /* ITD    */)

/* ---------- vCPU register helpers ----------
 *
 * Thin wrappers around hv_vcpu_{get,set}_reg/sys_reg with internal
 * HV_CHECK. Reduces boilerplate and centralizes error handling.
 * Use these for new code; existing call sites can migrate gradually. */

static inline uint64_t vcpu_get_gpr(hv_vcpu_t vcpu, unsigned n) {
    uint64_t val;
    HV_CHECK(hv_vcpu_get_reg(vcpu, HV_REG_X0 + n, &val));
    return val;
}

static inline void vcpu_set_gpr(hv_vcpu_t vcpu, unsigned n, uint64_t val) {
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0 + n, val));
}

static inline uint64_t vcpu_get_reg(hv_vcpu_t vcpu, hv_reg_t reg) {
    uint64_t val;
    HV_CHECK(hv_vcpu_get_reg(vcpu, reg, &val));
    return val;
}

static inline void vcpu_set_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t val) {
    HV_CHECK(hv_vcpu_set_reg(vcpu, reg, val));
}

static inline uint64_t vcpu_get_sysreg(hv_vcpu_t vcpu, hv_sys_reg_t reg) {
    uint64_t val;
    HV_CHECK(hv_vcpu_get_sys_reg(vcpu, reg, &val));
    return val;
}

static inline void vcpu_set_sysreg(hv_vcpu_t vcpu, hv_sys_reg_t reg,
                                    uint64_t val) {
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, reg, val));
}

/* ---------- Lock ordering ----------
 *
 * When acquiring multiple locks, always follow this total order to
 * prevent deadlock. Lower-numbered locks are acquired first.
 *
 *   1. mmap_lock     (syscall.c)       — mmap/brk/mprotect allocator
 *   2. pt_lock       (guest.c)         — page table pool allocation
 *   3. fd_lock       (syscall.c)       — FD table operations
 *   4. sig_lock      (syscall_signal.c)— signal state (pending/blocked/actions)
 *   5. thread_lock   (thread.c)        — thread table
 *   6. pid_lock      (syscall_proc.c)  — guest PID allocation
 *   7. futex buckets (futex.c)         — 64 per-address hash buckets
 *
 * Current nesting:
 *   mmap_lock → pt_lock  (sys_brk/sys_mmap/sys_mprotect → pt_alloc_page)
 *
 * All other locks are leaf locks (never held while acquiring another).
 * Futex buckets use strict index ordering (lower index first) when
 * operating on two buckets simultaneously (requeue, wake_op). */

#endif /* HV_UTIL_H */
