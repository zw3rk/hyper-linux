/* vdso.h — Guest vDSO + [vvar] time page for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * vDSO ELF at VDSO_BASE (RX) with real gettimeofday/clock_gettime that
 * seqlock-read host-published times from [vvar] at VVAR_BASE (RO to EL0,
 * host-written via host_base). rt_sigreturn remains an SVC trampoline at
 * a fixed offset (signal restorer).
 */
#ifndef VDSO_H
#define VDSO_H

#include "guest.h"
#include <stdint.h>

/* Guest addresses (identity-mapped low GPA in first 2MB RX block) */
#define VVAR_BASE     0x0000E000ULL
#define VVAR_SIZE     0x00001000ULL  /* 4KB */
#define VDSO_BASE     0x0000F000ULL
#define VDSO_SIZE     0x00001000ULL  /* 4KB */

/* .text start within the vDSO page (after ELF headers) */
#define VDSO_OFF_TEXT 0x0B0

/* Symbol offsets from VDSO_OFF_TEXT (nm -n on vdso_guest.o; keep in sync) */
#define VDSO_SYM_RT_SIGRETURN   0x00
#define VDSO_SYM_CLOCK_GETRES   0x0C
#define VDSO_SYM_CLOCK_GETTIME  0x18
#define VDSO_SYM_GETTIMEOFDAY   0xA8
#define VDSO_GUEST_TEXT_SIZE    0xF8

/* Guest-visible time data (host publishes; guest vDSO reads). LE. */
typedef struct hl_vvar {
    uint32_t seq;            /* seqlock: odd = write in progress */
    uint32_t version;        /* = HL_VVAR_VERSION when ready */
    int64_t  realtime_sec;
    int64_t  realtime_nsec;
    int64_t  mono_sec;
    int64_t  mono_nsec;
    uint64_t reserved[4];
} hl_vvar_t;

#define HL_VVAR_VERSION 1

/* Build vDSO ELF image at VDSO_BASE; zero/init [vvar]. Returns VDSO_BASE or 0. */
uint64_t vdso_build(guest_t *g);

/* Publish current host REALTIME + MONOTONIC into guest [vvar] (seqlock). */
void vdso_vvar_update(guest_t *g);

/* Start/stop 1ms host publisher for the active VM (covers pure gettimeofday loops). */
void vdso_publisher_start(guest_t *g);
void vdso_publisher_stop(void);

#endif /* VDSO_H */
