/* raw-syscall.h — Portable inline syscall wrappers for aarch64 and x86_64
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provides raw_syscall{1,3,6}() plus convenience wrappers (raw_clone,
 * raw_futex_wait, raw_futex_wake, raw_gettid, raw_getpid, raw_tgkill,
 * raw_exit).  Architecture is selected at compile time via __aarch64__
 * / __x86_64__ predefined macros.
 *
 * Syscall numbers come from <sys/syscall.h> (__NR_*), which already
 * provides the correct values per architecture.
 */
#ifndef RAW_SYSCALL_H
#define RAW_SYSCALL_H

#include <sys/syscall.h>
#include <linux/futex.h>

/* ── 1-argument syscall ─────────────────────────────────────────── */

static inline long raw_syscall1(long nr, long a0) {
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8)
                     : "memory", "cc");
    return x0;
#elif defined(__x86_64__)
    long ret;
    register long r10 __asm__("r10");
    (void)r10;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0)
                     : "rcx", "r11", "memory", "cc");
    return ret;
#else
#error "Unsupported architecture"
#endif
}

/* ── 2-argument syscall ─────────────────────────────────────────── */

static inline long raw_syscall2(long nr, long a0, long a1) {
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x8)
                     : "memory", "cc");
    return x0;
#elif defined(__x86_64__)
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1)
                     : "rcx", "r11", "memory", "cc");
    return ret;
#else
#error "Unsupported architecture"
#endif
}

/* ── 3-argument syscall ─────────────────────────────────────────── */

static inline long raw_syscall3(long nr, long a0, long a1, long a2) {
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x8)
                     : "memory", "cc");
    return x0;
#elif defined(__x86_64__)
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory", "cc");
    return ret;
#else
#error "Unsupported architecture"
#endif
}

/* ── 6-argument syscall ─────────────────────────────────────────── */

static inline long raw_syscall6(long nr, long a0, long a1, long a2,
                                long a3, long a4, long a5) {
#if defined(__aarch64__)
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3),
                       "r"(x4), "r"(x5), "r"(x8)
                     : "memory", "cc");
    return x0;
#elif defined(__x86_64__)
    long ret;
    register long r10 __asm__("r10") = a3;
    register long r8  __asm__("r8")  = a4;
    register long r9  __asm__("r9")  = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory", "cc");
    return ret;
#else
#error "Unsupported architecture"
#endif
}

/* ── Convenience wrappers ───────────────────────────────────────── */

static inline long raw_clone(unsigned long flags, void *child_stack,
                             int *ptid, unsigned long tls, int *ctid) {
    return raw_syscall6(__NR_clone, (long)flags, (long)child_stack,
                        (long)ptid, (long)tls, (long)ctid, 0);
}

static inline long raw_futex_wait(int *addr, int val) {
    return raw_syscall6(__NR_futex, (long)addr,
                        FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                        (long)val, 0, 0, 0);
}

static inline long raw_futex_wake(int *addr, int count) {
    return raw_syscall6(__NR_futex, (long)addr,
                        FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                        (long)count, 0, 0, 0);
}

static inline long raw_gettid(void) {
    return raw_syscall1(__NR_gettid, 0);
}

static inline long raw_getpid(void) {
    return raw_syscall1(__NR_getpid, 0);
}

static inline long raw_tgkill(int tgid, int tid, int sig) {
    return raw_syscall3(__NR_tgkill, (long)tgid, (long)tid, (long)sig);
}

static inline long raw_exit(int code) {
    return raw_syscall6(__NR_exit, (long)code, 0, 0, 0, 0, 0);
}

#endif /* RAW_SYSCALL_H */
