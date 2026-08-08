/* test-vvar-protect.c — Guest cannot forge [vvar] or hl-internal pages
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression guard for V2 (guest-integrity hardening of block 0 / pt-pool).
 *
 * hl places [vvar]/[vdso]/shim in the low 2MB block (VA 0..0x1FFFFF). The
 * stage-1 page-table pool lives at the high end of the primary buffer
 * (guest_size - 16MB). Before V2, block 0 was mapped RO/RX but a guest EL0
 * write raised a permission fault that the shim's data-abort handler treated
 * as a JIT W^X demand-toggle, silently promoting the page to RW. A guest
 * could thus forge its vDSO clock.
 *
 * Expected hardened behaviour:
 *   - [vvar] is still EL0-READABLE (the vDSO clock fast path reads it),
 *   - writing [vvar] faults (SIGSEGV/SIGBUS),
 *   - the former low pool range (0x10000..) stays unmapped (read/write fault),
 *   - the live high pool (36-bit IPA: last 16MB of 64GB) is not EL0-accessible.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <stdint.h>
#include <sys/auxv.h>
#include "test-harness.h"

/* Former low pool / hole just above [vdso]; must remain unmapped to EL0. */
#define LOW_HOLE_BASE 0x00010000ULL
/* 36-bit IPA default: pool is the last 16MB of the 64GB primary buffer.
 * A wrong mapping here would let the guest rewrite live stage-1 tables. */
#define HIGH_POOL_PROBE (0x1000000000ULL - 0x01000000ULL)

int passes = 0, fails = 0;

static sigjmp_buf jb;
static volatile sig_atomic_t caught;

static void fault_handler(int sig, siginfo_t *info, void *uc) {
    (void)sig; (void)info; (void)uc;
    caught = 1;
    siglongjmp(jb, 1);
}

/* Returns 1 if the access faulted (SIGSEGV/SIGBUS), 0 if it completed. */
static int try_read(volatile uint64_t *p) {
    caught = 0;
    if (sigsetjmp(jb, 1) == 0) { volatile uint64_t v = *p; (void)v; return 0; }
    return 1;
}
static int try_write(volatile uint64_t *p) {
    caught = 0;
    if (sigsetjmp(jb, 1) == 0) { *p = 0xDEADBEEFCAFEF00DULL; return 0; }
    return 1;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    unsigned long ehdr = getauxval(AT_SYSINFO_EHDR);   /* == VDSO_BASE */
    volatile uint64_t *vvar = (volatile uint64_t *)(ehdr ? ehdr - 0x1000 : 0xE000ULL);
    volatile uint64_t *low_hole = (volatile uint64_t *)LOW_HOLE_BASE;
    volatile uint64_t *high_pool = (volatile uint64_t *)HIGH_POOL_PROBE;

    /* Positive control: [vvar] must remain EL0-readable for the vDSO clock. */
    TEST("[vvar] stays EL0-readable");
    if (!try_read(vvar)) PASS();
    else FAIL("[vvar] read faulted — vDSO clock fast path would break");

    TEST("[vvar] write faults");
    if (try_write(vvar)) PASS();
    else FAIL("[vvar] write succeeded — guest can forge the vDSO clock");

    TEST("low hole (former pt-pool) read faults");
    if (try_read(low_hole)) PASS();
    else FAIL("low hole read succeeded — expected unmapped after pool relocate");

    TEST("high pt-pool write faults");
    if (try_write(high_pool)) PASS();
    else FAIL("high pt-pool write succeeded — guest can rewrite its translation");

    SUMMARY("test-vvar-protect");
    return fails ? 1 : 0;
}
