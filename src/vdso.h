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

/* Harden hl-internal pages against EL0 tampering.
 * Call once after the page tables are built and the null-guard has split
 * block 0 into L3 pages (guest_invalidate_ptes(g, 0, 0x1000)):
 *   - page-table pool (high end of primary buffer, see guest_page_table_pool_*):
 *     stage-1 invalid (no EL0 access; the MMU still walks it by IPA via stage-2),
 *   - holes below [vvar] and the former low pool range up to SHIM_BASE:
 *     stage-1 invalid,
 *   - [vvar]: EL0 read-only + execute-never,
 *   - [vdso]: left RX (EL0 read + execute).
 * EL0 *writes* to the still-mapped [vvar]/[vdso]/shim pages are rejected by
 * the shim's block-0 permission-fault guard (SIGSEGV, not a W^X toggle). */
void vdso_harden_low_block(guest_t *g);

/* Publish current host REALTIME + MONOTONIC into guest [vvar] (seqlock). */
void vdso_vvar_update(guest_t *g);

/* Start/stop 1ms host publisher for the active VM (covers pure gettimeofday loops). */
void vdso_publisher_start(guest_t *g);
void vdso_publisher_stop(void);

#endif /* VDSO_H */
