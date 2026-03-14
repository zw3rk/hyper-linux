/* test-sigill.c — Test SIGILL delivery for undefined instructions
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 * 1. Executing UDF #0 (permanently undefined) delivers SIGILL
 * 2. si_signo == SIGILL and si_code == ILL_ILLOPC
 * 3. Handler can recover (sigreturn advances past the faulting insn)
 * 4. NULL dereference produces SIGSEGV (not SIGILL from zero-page code)
 */
#define _GNU_SOURCE   /* REG_RIP on musl/glibc x86_64 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t sigill_caught = 0;
static volatile int sigill_si_code = 0;
static volatile sig_atomic_t sigsegv_caught = 0;
static volatile int sigsegv_si_code = 0;

#if defined(__aarch64__)

static void sigill_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    sigill_caught = 1;
    sigill_si_code = info->si_code;

    /* Advance PC past the UDF instruction (4 bytes) to resume execution.
     * ucontext_t.uc_mcontext.__ss.__pc on aarch64 Linux is the faulting PC. */
    ucontext_t *uc = (ucontext_t *)ucontext;
    /* Linux aarch64: mcontext_t has regs[31], sp, pc, pstate.
     * pc is at offset regs[32] equivalent, but the layout varies.
     * Use the struct field directly. */
    uc->uc_mcontext.pc += 4;
}

static void sigsegv_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)ucontext;
    sigsegv_caught = 1;
    sigsegv_si_code = info->si_code;
    /* Cannot safely resume from SIGSEGV — just record and longjmp-style exit.
     * Use _exit-equivalent via re-raising with default handler. But for test
     * simplicity, we'll set a flag and let main check it via setjmp/sigsetjmp. */
}

#elif defined(__x86_64__)

static void sigill_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    sigill_caught = 1;
    sigill_si_code = info->si_code;

    /* Advance RIP past the UD2 instruction (2 bytes) */
    ucontext_t *uc = (ucontext_t *)ucontext;
    uc->uc_mcontext.gregs[REG_RIP] += 2;
}

static void sigsegv_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)ucontext;
    sigsegv_caught = 1;
    sigsegv_si_code = info->si_code;
}

#endif

/* ILL_ILLOPC is defined as 1 on Linux */
#ifndef ILL_ILLOPC
#define ILL_ILLOPC 1
#endif

#include <setjmp.h>
static sigjmp_buf segv_jmp;

static void sigsegv_handler_jmp(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)ucontext;
    sigsegv_caught = 1;
    sigsegv_si_code = info->si_code;
    siglongjmp(segv_jmp, 1);
}

int main(void) {
    int pass = 0, fail = 0;

    /* Test 1: SIGILL from UDF/UD2 instruction */
    printf("test-sigill: test 1: SIGILL from undefined instruction... ");
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = sigill_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGILL, &sa, NULL) < 0) {
            printf("FAIL (sigaction: %m)\n");
            fail++;
        } else {
            sigill_caught = 0;
            sigill_si_code = 0;

#if defined(__aarch64__)
            /* UDF #0: permanently undefined instruction, encoding 0x00000000 */
            __asm__ volatile(".word 0x00000000");
#elif defined(__x86_64__)
            /* UD2: undefined instruction, encoding 0x0F 0x0B */
            __asm__ volatile("ud2");
#endif

            if (sigill_caught && sigill_si_code == ILL_ILLOPC) {
                printf("PASS\n");
                pass++;
            } else {
                printf("FAIL (caught=%d si_code=%d, expected ILL_ILLOPC=%d)\n",
                       (int)sigill_caught, sigill_si_code, ILL_ILLOPC);
                fail++;
            }
        }
    }

    /* Test 2: NULL dereference produces SIGSEGV (not SIGILL) */
    printf("test-sigill: test 2: NULL deref → SIGSEGV... ");
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = sigsegv_handler_jmp;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGSEGV, &sa, NULL) < 0) {
            printf("FAIL (sigaction: %m)\n");
            fail++;
        } else {
            sigsegv_caught = 0;
            sigsegv_si_code = 0;

            if (sigsetjmp(segv_jmp, 1) == 0) {
                /* Attempt to read from address 0 — should fault */
                volatile int *null_ptr = (volatile int *)0;
                (void)*null_ptr;
                /* If we reach here, SIGSEGV was not delivered */
                printf("FAIL (no SIGSEGV)\n");
                fail++;
            } else {
                /* Returned from siglongjmp in SIGSEGV handler */
                if (sigsegv_caught) {
                    printf("PASS (si_code=%d)\n", sigsegv_si_code);
                    pass++;
                } else {
                    printf("FAIL (handler not called)\n");
                    fail++;
                }
            }
        }
    }

    printf("\ntest-sigill: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
