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

/* ---------- VZ ioctl protocol ---------- */

/* Rosetta VZ (Virtualization.framework) mode activation ioctls.
 * Three ioctls on the rosetta fd: check for VZ support, query
 * capabilities, and activate VZ mode.  Intercepted in sys_ioctl. */
#define ROSETTA_VZ_CHECK     0x80456125   /* Returns 69-byte signature */
#define ROSETTA_VZ_CAPS      0x80806123   /* Returns 128-byte capability data */
#define ROSETTA_VZ_ACTIVATE  0x6124       /* Activate VZ mode */

/* VZ_CAPS buffer layout (128 bytes).  Filled by hl to emulate a VZ
 * environment so rosetta enables its AOT translation path. */
#define ROSETTA_CAPS_SIZE             128  /* Total buffer size */
#define ROSETTA_CAPS_VZ_ENABLE        0   /* caps[0]: 1 = VZ active */
#define ROSETTA_CAPS_SOCKET_PATH      1   /* caps[1..64]: rosettad socket path */
#define ROSETTA_CAPS_SOCKET_PATH_LEN  64  /* Max socket path length */
#define ROSETTA_CAPS_BINARY_PATH      66  /* caps[66..107]: x86_64 binary path */
#define ROSETTA_CAPS_BINARY_PATH_LEN  42  /* Max binary path length */
#define ROSETTA_CAPS_VZ_SECONDARY     108 /* caps[108]: secondary VZ flag */

/* VZ_CHECK signature (reverse-engineered from rosetta binary) */
#define ROSETTA_VZ_SIG_LEN  69

/* ---------- rosettad protocol ---------- */

/* Protocol commands sent over the rosettad socketpair.  Rosetta opens
 * AF_UNIX SOCK_SEQPACKET; hl intercepts with socketpair(SOCK_STREAM)
 * and runs a handler thread implementing this protocol. */
#define ROSETTAD_CMD_HANDSHAKE  '?'   /* Ready check → respond 0x01 */
#define ROSETTAD_CMD_TRANSLATE  't'   /* AOT translate: binary fd via SCM_RIGHTS */
#define ROSETTAD_CMD_DIGEST     'd'   /* Digest lookup: 32-byte SHA256 */
#define ROSETTAD_CMD_QUIT       'q'   /* Quit handler thread */

/* Protocol responses */
#define ROSETTAD_RESP_HIT   0x01      /* Cache hit / success */
#define ROSETTAD_RESP_MISS  0x00      /* Cache miss / failure */

/* SHA256 digest constants (used for AOT persistent cache) */
#define ROSETTAD_DIGEST_SIZE     32                 /* SHA256 digest bytes */
#define ROSETTAD_DIGEST_HEX_LEN (ROSETTAD_DIGEST_SIZE * 2 + 1)  /* Hex + NUL */

/* AOT translation limits */
#define ROSETTAD_BINARY_SIZE_LIMIT  (100LL * 1024 * 1024)  /* Skip >100MB binaries */

/* Persistent AOT cache directory (under $HOME) */
#define ROSETTAD_CACHE_SUBDIR  ".cache/hl-rosettad"

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
