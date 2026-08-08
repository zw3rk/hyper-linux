/* vdso_guest.S — Guest EL0 vDSO text blob
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Assembled with: clang -arch arm64 -c
 * Offsets of global symbols are recorded in vdso.h (VDSO_SYM_*).
 * First symbol MUST remain rt_sigreturn (signal restorer at VDSO_OFF_TEXT).
 */
    .text
    .align 2

/* ---- rt_sigreturn (SVC) ---- */
    .global _vdso_sym_rt_sigreturn
_vdso_sym_rt_sigreturn:
    mov     x8, #139
    svc     #0
    ret

/* ---- clock_getres (SVC, rare) ---- */
    .global _vdso_sym_clock_getres
_vdso_sym_clock_getres:
    mov     x8, #114
    svc     #0
    ret

/* ---- clock_gettime(clockid, tp) via [vvar] ---- */
    .global _vdso_sym_clock_gettime
_vdso_sym_clock_gettime:
    cmp     x0, #0
    b.eq    Lcg_real
    cmp     x0, #5
    b.eq    Lcg_real
    cmp     x0, #1
    b.eq    Lcg_mono
    cmp     x0, #6
    b.eq    Lcg_mono
    cmp     x0, #7
    b.eq    Lcg_mono
    mov     x8, #113
    svc     #0
    ret

Lcg_real:
    mov     w9, #1
    b       Lcg_body
Lcg_mono:
    mov     w9, #0
Lcg_body:
    movz    x10, #0xE000
Lcg_retry:
    ldr     w11, [x10]
    tbnz    w11, #0, Lcg_retry
    ldr     w12, [x10, #4]
    cmp     w12, #1
    b.ne    Lcg_svc
    cbnz    w9, Lcg_load_real
    ldp     x13, x14, [x10, #24]
    b       Lcg_check
Lcg_load_real:
    ldp     x13, x14, [x10, #8]
Lcg_check:
    ldr     w12, [x10]
    cmp     w11, w12
    b.ne    Lcg_retry
    /* Rosetta routes x86_64 clock_gettime syscalls through this helper.
     * Fall back to the real syscall for NULL/non-canonical result pointers so
     * the kernel-side guest_write check returns EFAULT instead of taking an
     * EL0 fault while the vDSO stores the timespec. */
    cbz     x1, Lcg_svc
    lsr     x15, x1, #48
    cbnz    x15, Lcg_svc
    stp     x13, x14, [x1]
Lcg_ok:
    mov     x0, xzr
    ret
Lcg_svc:
    mov     x8, #113
    svc     #0
    ret

/* ---- gettimeofday(tv, tz) via [vvar] ---- */
    .global _vdso_sym_gettimeofday
_vdso_sym_gettimeofday:
    movz    x10, #0xE000
Lgtod_retry:
    ldr     w11, [x10]
    tbnz    w11, #0, Lgtod_retry
    ldr     w12, [x10, #4]
    cmp     w12, #1
    b.ne    Lgtod_svc
    ldp     x13, x14, [x10, #8]
    ldr     w12, [x10]
    cmp     w11, w12
    b.ne    Lgtod_retry
    cbz     x0, Lgtod_ok
    str     x13, [x0]
    mov     x15, #1000
    udiv    x14, x14, x15
    str     x14, [x0, #8]
Lgtod_ok:
    mov     x0, xzr
    ret
Lgtod_svc:
    mov     x8, #169
    svc     #0
    ret

    .global _vdso_guest_text_end
_vdso_guest_text_end:
