/* rosetta.h — Rosetta x86_64-linux translator setup for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two-phase API for initializing rosetta in the guest VM:
 *   Phase 1: rosetta_prepare()  — load binary, place segments, build regions
 *   Phase 2: rosetta_finalize() — kbuf dual-mapping, vDSO, argv, proc state
 *
 * The split matches the guest_build_page_tables() boundary: phase 1 runs
 * before page tables are built (appends regions), phase 2 runs after
 * (creates non-identity mappings in the built page tables).
 */
#ifndef ROSETTA_H
#define ROSETTA_H

#include <Hypervisor/Hypervisor.h>
#include "guest.h"
#include "elf.h"

/* ---------- TCR_EL1 ---------- */

/* TCR_EL1 for 4KB granule, 48-bit VA:
 *   T0SZ  = 16  (48-bit TTBR0 VA space)
 *   IRGN0 = WB RA WA, ORGN0 = WB RA WA, SH0 = Inner Shareable
 *   TG0   = 4KB
 *   T1SZ  = 16  (48-bit TTBR1 VA space)
 *   EPD1  = 1   (DISABLE TTBR1 walks — cleared for rosetta mode)
 *   IRGN1 = WB RA WA, ORGN1 = WB RA WA, SH1 = Inner Shareable
 *   TG1   = 4KB (encoding 0b10)
 *   IPS   = 48-bit PA (0b101)
 *   TBI0  = 1   (Top Byte Ignore for TTBR0)
 *   TBI1  = 1   (Top Byte Ignore for TTBR1, rosetta mode only) */
#define TCR_EL1_VALUE       0x25B5903510ULL  /* EPD1=1: TTBR1 walks disabled */
#define TCR_EL1_VALUE_KBUF  0x65B5103510ULL  /* EPD1=0, TBI1=1: TTBR1 + TBI */

/* ---------- Result structure ---------- */

/* Output from rosetta_prepare(), consumed by rosetta_finalize() and
 * the caller (for build_linux_stack auxv). */
typedef struct {
    uint64_t    entry_point;   /* Rosetta's ELF entry (high VA) */
    elf_info_t  rosetta_info;  /* Parsed rosetta ELF (for stack auxv) */
} rosetta_result_t;

/* ---------- Phase 1: before guest_build_page_tables() ---------- */

/* Load the rosetta binary, place segments in the primary buffer, set up
 * VA aliases and kbuf, and append page table regions for rosetta segments
 * and the vDSO.
 *
 * binary_path: realpath of the x86_64 binary being executed.
 * regions/nregions: caller's page table region array (entries appended).
 * max_regions: capacity of regions[].
 * verbose: enable debug logging.
 * result: output rosetta ELF info + entry point.
 *
 * On first call (g->rosetta_guest_base == 0): full setup — loads segments,
 * creates VA aliases, initializes kbuf.
 * On re-call (execve): reloads segments into existing placement, zeros kbuf.
 *
 * Returns 0 on success, -1 on failure. */
int rosetta_prepare(guest_t *g, const char *binary_path,
                    mem_region_t *regions, int *nregions, int max_regions,
                    int verbose, rosetta_result_t *result);

/* ---------- Phase 2: after guest_build_page_tables() ---------- */

/* Complete rosetta setup: kbuf user VA dual-mapping, TCR/TTBR1 sysregs,
 * vDSO build, semantic regions, pre-open binary at fd 3, build binfmt_misc
 * argv, and set proc state (elf_path, cmdline, rosettad binary path).
 *
 * guest_argc/guest_argv: the original (user-facing) argc/argv.
 * rr: result from rosetta_prepare().
 *
 * Outputs:
 *   out_argc/out_argv: constructed rosetta argv (caller must free *out_argv).
 *   out_vdso_addr: vDSO base address for AT_SYSINFO_EHDR.
 *
 * Returns 0 on success, -1 on failure. */
int rosetta_finalize(guest_t *g, hv_vcpu_t vcpu,
                     const char *binary_path,
                     int guest_argc, const char **guest_argv,
                     const rosetta_result_t *rr,
                     int verbose,
                     int *out_argc, const char ***out_argv,
                     uint64_t *out_vdso_addr);

#endif /* ROSETTA_H */
