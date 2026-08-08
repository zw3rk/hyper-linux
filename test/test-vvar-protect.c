/* test-vvar-protect.c — Guest cannot forge [vvar] or hl's page-table pool
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression guard for V2 (guest-integrity hardening of block 0).
 *
 * hl backs the low 2MB block (VA 0..0x1FFFFF) with hl-internal state: the
 * stage-1 page-table pool (PT_POOL_BASE 0x10000 .. PT_POOL_END 0x100000),
 * the [vvar] time page ([vdso] page - 0x1000), [vdso] code, and the shim.
 * Before the fix, block 0 was mapped RO/RX but a guest EL0 write raised a
 * permission fault that the shim's data-abort handler treated as a JIT W^X
 * demand-toggle, silently promoting the page to RW so the write landed. A
 * guest could thus forge its vDSO clock or (writing pt-pool page 0x10000,
 * the live L0 table) rewrite its own translation.
 *
 * Expected hardened behaviour:
 *   - [vvar] is still EL0-READABLE (the vDSO clock fast path reads it),
 *   - writing [vvar] faults (SIGSEGV/SIGBUS),
 *   - reading the page-table pool faults (no EL0 access at all),
 *   - writing the page-table pool faults.
 *
 * These assertions fault cleanly whether or not the fix is present (the
 * negative case reads/writes memory that is not a live translation table),
 * so the test is mutation-verifiable without hanging: revert the hardening
 * and the write/read that used to "succeed" now shows up as a FAIL here.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <stdint.h>
#include <sys/auxv.h>
#include "test-harness.h"

/* hl guest layout constants (src/guest.h PT_POOL_*, src/vdso.h VVAR/VDSO). */
#define PT_POOL_BASE 0x00010000ULL
#define PT_POOL_END  0x00100000ULL

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
    volatile uint64_t *pt_l0 = (volatile uint64_t *)PT_POOL_BASE;          /* live L0 table */
    /* High, normally-unused pool page: a write here cannot corrupt a live
     * translation table if the hardening is (mutation-)reverted, so the
     * negative path stays clean instead of hanging. */
    volatile uint64_t *pt_hi = (volatile uint64_t *)(PT_POOL_END - 0x1000ULL);

    /* Positive control: [vvar] must remain EL0-readable for the vDSO clock. */
    TEST("[vvar] stays EL0-readable");
    if (!try_read(vvar)) PASS();
    else FAIL("[vvar] read faulted — vDSO clock fast path would break");

    TEST("[vvar] write faults");
    if (try_write(vvar)) PASS();
    else FAIL("[vvar] write succeeded — guest can forge the vDSO clock");

    TEST("pt-pool read faults");
    if (try_read(pt_l0)) PASS();
    else FAIL("pt-pool read succeeded — guest can read hl's page tables");

    TEST("pt-pool write faults");
    if (try_write(pt_hi)) PASS();
    else FAIL("pt-pool write succeeded — guest can rewrite its translation");

    SUMMARY("test-vvar-protect");
    return fails ? 1 : 0;
}
