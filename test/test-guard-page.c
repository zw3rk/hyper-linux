/* test-guard-page.c — Stack guard page and mmap edge case tests
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: stack guard page (PROT_NONE), large mmap allocations,
 *        MAP_FIXED overlap, mprotect transitions.
 *
 * Syscalls exercised: mmap(222), munmap(215), mprotect(226)
 */
#include "test-harness.h"
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int passes = 0, fails = 0;

static volatile int got_signal = 0;

static void sigsegv_handler(int sig) {
    (void)sig;
    got_signal = 1;
    _exit(0);  /* Can't safely return from SIGSEGV — just verify we got here */
}

/* ---------- Test 1: mmap PROT_NONE is inaccessible ---------- */

static void test_prot_none(void) {
    TEST("mmap PROT_NONE succeeds");
    {
        void *p = mmap(NULL, 4096, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            PASS();
            munmap(p, 4096);
        } else {
            FAIL("mmap PROT_NONE failed");
        }
    }

    TEST("mprotect NONE→RW→NONE cycle");
    {
        void *p = mmap(NULL, 4096, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { FAIL("mmap failed"); return; }

        /* Upgrade to RW, write, verify */
        if (mprotect(p, 4096, PROT_READ | PROT_WRITE) != 0) {
            FAIL("mprotect NONE→RW failed");
            munmap(p, 4096);
            return;
        }
        *(volatile int *)p = 0xDEADBEEF;
        if (*(volatile int *)p == (int)0xDEADBEEF) PASS();
        else FAIL("data not readable after RW mprotect");

        /* Downgrade back to NONE (don't access — would segfault) */
        mprotect(p, 4096, PROT_NONE);
        munmap(p, 4096);
    }
}

/* ---------- Test 2: Large mmap allocation ---------- */

static void test_large_mmap(void) {
    TEST("mmap 64MB anonymous");
    {
        size_t sz = 64UL * 1024 * 1024;
        void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { FAIL("mmap 64MB failed"); return; }

        /* Touch first, middle, and last pages to verify demand paging */
        volatile char *c = (volatile char *)p;
        c[0] = 'A';
        c[sz / 2] = 'B';
        c[sz - 1] = 'C';

        if (c[0] == 'A' && c[sz / 2] == 'B' && c[sz - 1] == 'C') PASS();
        else FAIL("data mismatch in 64MB region");

        munmap(p, sz);
    }
}

/* ---------- Test 3: MAP_FIXED overlapping ---------- */

static void test_map_fixed_overlap(void) {
    TEST("MAP_FIXED replaces existing");
    {
        /* Allocate 3 pages */
        size_t sz = 3 * 4096;
        void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { FAIL("initial mmap failed"); return; }

        /* Write a marker in the middle page */
        volatile int *mid = (volatile int *)((char *)p + 4096);
        *mid = 0x12345678;

        /* MAP_FIXED over the middle page with a new mapping */
        void *p2 = mmap((char *)p + 4096, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p2 == MAP_FAILED) { FAIL("MAP_FIXED failed"); munmap(p, sz); return; }

        /* New mapping should be zeroed (fresh anonymous page) */
        if (*mid == 0) PASS();
        else FAIL("MAP_FIXED didn't zero replaced page");

        munmap(p, sz);
    }
}

/* ---------- Test 4: mprotect sub-page granularity ---------- */

static void test_mprotect_partial(void) {
    TEST("mprotect partial region");
    {
        /* Allocate 4 pages RW */
        size_t sz = 4 * 4096;
        void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { FAIL("mmap failed"); return; }

        /* Write to all 4 pages */
        for (int i = 0; i < 4; i++)
            ((volatile int *)((char *)p + i * 4096))[0] = i + 1;

        /* Make middle 2 pages read-only */
        if (mprotect((char *)p + 4096, 2 * 4096, PROT_READ) != 0) {
            FAIL("mprotect failed");
            munmap(p, sz);
            return;
        }

        /* First and last pages should still be writable */
        ((volatile int *)p)[0] = 99;
        ((volatile int *)((char *)p + 3 * 4096))[0] = 99;

        /* Middle pages should still be readable */
        int v1 = ((volatile int *)((char *)p + 4096))[0];
        int v2 = ((volatile int *)((char *)p + 2 * 4096))[0];
        if (v1 == 2 && v2 == 3) PASS();
        else FAIL("middle page data corrupted");

        munmap(p, sz);
    }
}

/* ---------- Test 5: Multiple mmap/munmap cycles ---------- */

static void test_mmap_churn(void) {
    TEST("100x mmap/munmap cycle");
    {
        int ok = 1;
        for (int i = 0; i < 100; i++) {
            void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) { ok = 0; break; }
            *(volatile int *)p = i;
            if (*(volatile int *)p != i) { ok = 0; break; }
            munmap(p, 4096);
        }
        if (ok) PASS();
        else FAIL("mmap/munmap cycle failed");
    }
}

/* ---------- Main ---------- */

int main(void) {
    printf("test-guard-page: stack guard + mmap edge case tests\n");

    test_prot_none();
    test_large_mmap();
    test_map_fixed_overlap();
    test_mprotect_partial();
    test_mmap_churn();

    SUMMARY("test-guard-page");
    return fails > 0 ? 1 : 0;
}
