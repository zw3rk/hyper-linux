/* stack.c — Linux initial stack builder for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Constructs the initial stack layout that the Linux ABI requires at
 * process startup. The stack is built at the top of the guest stack
 * region and grows downward: string data at the top, then the structured
 * area (auxv, envp, argv, argc) below.
 */
#include "stack.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/random.h>

/* ---------- Linux aarch64 HWCAP bits (from asm/hwcap.h) ---------- */
#define HWCAP_FP        (1ULL << 0)
#define HWCAP_ASIMD     (1ULL << 1)
#define HWCAP_EVTSTRM   (1ULL << 2)
#define HWCAP_AES       (1ULL << 3)
#define HWCAP_PMULL     (1ULL << 4)
#define HWCAP_SHA1      (1ULL << 5)
#define HWCAP_SHA2      (1ULL << 6)
#define HWCAP_CRC32     (1ULL << 7)
#define HWCAP_ATOMICS   (1ULL << 8)   /* LSE atomics */
#define HWCAP_FPHP      (1ULL << 9)   /* FP16 */
#define HWCAP_ASIMDHP   (1ULL << 10)  /* ASIMD FP16 */
#define HWCAP_ASIMDRDM  (1ULL << 12)
#define HWCAP_JSCVT     (1ULL << 13)
#define HWCAP_FCMA      (1ULL << 14)
#define HWCAP_LRCPC     (1ULL << 15)
#define HWCAP_DCPOP     (1ULL << 16)
#define HWCAP_SHA3      (1ULL << 17)
#define HWCAP_SM3       (1ULL << 18)
#define HWCAP_SM4       (1ULL << 19)
#define HWCAP_ASIMDDP   (1ULL << 20)  /* Dot product */
#define HWCAP_SHA512    (1ULL << 21)
#define HWCAP_FHM       (1ULL << 23)
#define HWCAP_DIT       (1ULL << 24)
#define HWCAP_ILRCPC    (1ULL << 26)
#define HWCAP_FLAGM     (1ULL << 27)
#define HWCAP_CPUID     (1ULL << 11)  /* MRS emulation of ID registers from EL0 */
#define HWCAP_SSBS      (1ULL << 28)
#define HWCAP_SB        (1ULL << 29)
#define HWCAP_PACA      (1ULL << 30)
#define HWCAP_PACG      (1ULL << 31)

/* ---------- Linux aarch64 HWCAP2 bits (from asm/hwcap.h) ---------- */
#define HWCAP2_DCPODP   (1ULL << 0)
#define HWCAP2_FLAGM2   (1ULL << 7)
#define HWCAP2_FRINT    (1ULL << 8)
#define HWCAP2_BF16     (1ULL << 14)

/* Build AT_HWCAP value matching VZ-sanitized ID register values.
 *
 * These must be consistent with the VZ-sanitized ID register overrides
 * in syscall_proc.c (MRS trap handler). The Linux kernel derives HWCAP
 * from ID registers; rosetta reads both HWCAP and ID registers, so they
 * must agree. Inconsistencies cause rosetta to select wrong JIT paths.
 *
 * Derived from:
 *   ID_AA64ISAR0_EL1 = 0x0021100110212120
 *   ID_AA64ISAR1_EL1 = 0x0000101110211402
 *   ID_AA64PFR0_EL1  = 0x0001000000110011
 *   ID_AA64PFR1_EL1  = 0x0000000000000000
 *   ID_AA64MMFR2_EL1 = 0x0000000000000000
 */
static uint64_t query_hwcap(void) {
    uint64_t hwcap = HWCAP_FP | HWCAP_ASIMD |       /* PFR0.FP/AdvSIMD != 0xF */
                     HWCAP_AES | HWCAP_PMULL |       /* ISAR0.AES = 2 */
                     HWCAP_SHA1 |                    /* ISAR0.SHA1 = 1 */
                     HWCAP_SHA2 | HWCAP_SHA512 |     /* ISAR0.SHA2 = 2 */
                     HWCAP_CRC32 |                   /* ISAR0.CRC32 = 1 */
                     HWCAP_ATOMICS |                 /* ISAR0.Atomic = 2 */
                     HWCAP_FPHP | HWCAP_ASIMDHP |   /* PFR0.FP/AdvSIMD >= 1 */
                     HWCAP_CPUID |                   /* always (kernel emulates MRS) */
                     HWCAP_ASIMDRDM |                /* ISAR0.RDM = 1 */
                     HWCAP_JSCVT |                   /* ISAR1.JSCVT = 1 */
                     HWCAP_FCMA |                    /* ISAR1.FCMA = 1 */
                     HWCAP_LRCPC | HWCAP_ILRCPC |   /* ISAR1.LRCPC = 2 */
                     HWCAP_DCPOP |                   /* ISAR1.DPB = 2 */
                     HWCAP_SHA3 |                    /* ISAR0.SHA3 = 1 */
                     HWCAP_ASIMDDP |                 /* ISAR0.DP = 1 */
                     HWCAP_FHM |                     /* ISAR0.FHM = 1 */
                     HWCAP_DIT |                     /* PFR0.DIT = 1 */
                     HWCAP_FLAGM |                   /* ISAR0.TS = 2 */
                     HWCAP_SB |                      /* ISAR1.SB = 1 */
                     HWCAP_PACA |                    /* ISAR1.API = 4 */
                     HWCAP_PACG;                     /* ISAR1.GPI = 1 */
    /* NOT set:
     * HWCAP_SSBS  — PFR1.SSBS = 0 (PFR1 is all zeros in VZ)
     * HWCAP_EVTSTRM — no generic timer emulation
     * HWCAP_SVE   — PFR0.SVE = 0
     * HWCAP_SM3   — ISAR0.SM3 = 0
     * HWCAP_SM4   — ISAR0.SM4 = 0
     * HWCAP_USCAT — MMFR2.AT = 0
     */
    return hwcap;
}

/* Build AT_HWCAP2 value matching VZ-sanitized ID register values. */
static uint64_t query_hwcap2(void) {
    return HWCAP2_DCPODP |    /* ISAR1.DPB = 2 */
           HWCAP2_FLAGM2 |    /* ISAR0.TS = 2 */
           HWCAP2_FRINT |     /* ISAR1.FRINTTS = 1 */
           HWCAP2_BF16;       /* ISAR1.BF16 = 1 */
}

/* Push a uint64_t onto the stack (growing downward) */
static void push_u64(guest_t *g, uint64_t *sp, uint64_t val) {
    *sp -= 8;
    guest_write(g, *sp, &val, 8);
}

/* Write a string to guest memory at the given address, return length+1 */
static size_t write_str(guest_t *g, uint64_t gva, const char *s) {
    size_t len = strlen(s) + 1;
    guest_write(g, gva, s, len);
    return len;
}

uint64_t build_linux_stack(guest_t *g, uint64_t stack_top,
                           int argc, const char **argv,
                           const char **envp,
                           const elf_info_t *elf_info,
                           uint64_t elf_load_base,
                           uint64_t interp_base,
                           uint64_t vdso_base) {
    /*
     * Linux initial stack layout (growing from high to low):
     *   [ 16 random bytes for AT_RANDOM ]
     *   [ "aarch64\0" for AT_PLATFORM ]
     *   [ environment strings ]
     *   [ argument strings ]
     *   [ padding to 16-byte alignment ]
     *   [ AT_NULL (0, 0) ]
     *   [ auxv entries (key, value) pairs ]
     *   [ NULL (end of envp) ]
     *   [ envp[0], envp[1], ... ]
     *   [ NULL (end of argv) ]
     *   [ argv[argc-1] ... argv[0] ]
     *   [ argc ]                    <-- SP points here
     */

    /* Count environment entries */
    int envc = 0;
    if (envp) {
        while (envp[envc]) envc++;
    }

    /* Bounds-check: Linux returns E2BIG for oversized argument/environment.
     * ARG_MAX on Linux is typically 2MB; we cap at reasonable stack limits. */
    #define MAX_ARGS 131072
    #define MAX_ENVS 131072
    if (argc > MAX_ARGS || envc > MAX_ENVS)
        return 0; /* Caller treats 0 as failure */

    /* Phase 1: Write strings and random data at the top of the stack.
     * We work downward from stack_top. */
    uint64_t str_ptr = stack_top;

    /* AT_RANDOM: 16 random bytes */
    str_ptr -= 16;
    uint64_t random_ptr = str_ptr;
    uint8_t random_bytes[16];
    getentropy(random_bytes, 16);
    guest_write(g, random_ptr, random_bytes, 16);

    /* AT_PLATFORM: "aarch64\0" */
    str_ptr -= 8;  /* strlen("aarch64") + 1 */
    uint64_t platform_ptr = str_ptr;
    write_str(g, platform_ptr, "aarch64");

    /* Dynamically allocate pointer arrays to avoid stack buffer overflow
     * with large argument or environment lists. */
    uint64_t *env_ptrs = calloc((size_t)envc, sizeof(uint64_t));
    uint64_t *arg_ptrs = calloc((size_t)argc, sizeof(uint64_t));
    if ((envc > 0 && !env_ptrs) || (argc > 0 && !arg_ptrs)) {
        free(env_ptrs);
        free(arg_ptrs);
        return 0;
    }

    /* Environment strings */
    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        str_ptr -= len;
        env_ptrs[i] = str_ptr;
        write_str(g, str_ptr, envp[i]);
    }

    /* Argument strings (written backward so argv[0] is at lowest addr) */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        str_ptr -= len;
        arg_ptrs[i] = str_ptr;
        write_str(g, str_ptr, argv[i]);
    }

    /* AT_EXECFN: pointer to argv[0] string (write it near the top) */
    uint64_t execfn_ptr = (argc > 0) ? arg_ptrs[0] : 0;

    /* Phase 2: Build the structured part of the stack.
     * Align str_ptr down to 16 bytes first. */
    str_ptr &= ~15ULL;
    uint64_t sp = str_ptr;

    /* Count auxv entries: base 15 + optional AT_BASE + AT_EXECFN +
     * optional AT_SYSINFO_EHDR = up to 18.
     * Each auxv entry = 2 words. Plus AT_NULL = 2 words.
     * Total words from auxv = (num_auxv_entries + 1) * 2.
     * Plus envp_null(1) + envp_ptrs(envc) + argv_null(1) + argv_ptrs(argc) + argc(1).
     *
     * Base auxv: 15 entries = 30 words, AT_NULL = 2 words.
     * Optional: AT_BASE (if interp_base != 0) = +2, AT_EXECFN = +2,
     *           AT_SYSINFO_EHDR (if vdso_base != 0) = +2.
     * Total = 35 + optional + 1 + envc + 1 + argc + 1
     * For 16-byte alignment: total must be even. */
    int extra = 2;  /* AT_EXECFN always present */
    if (interp_base != 0) extra += 2;  /* AT_BASE */
    if (vdso_base != 0) extra += 2;    /* AT_SYSINFO_EHDR */
    int total_entries = 35 + extra + argc + envc;
    if (total_entries & 1) {
        push_u64(g, &sp, 0);  /* alignment padding */
    }

    /* Auxv entries (pushed in reverse order since stack grows down).
     * We push AT_NULL last (first in stack order). */
    push_u64(g, &sp, 0); push_u64(g, &sp, AT_NULL);

    push_u64(g, &sp, platform_ptr); push_u64(g, &sp, AT_PLATFORM);
    push_u64(g, &sp, execfn_ptr); push_u64(g, &sp, AT_EXECFN);
    push_u64(g, &sp, random_ptr); push_u64(g, &sp, AT_RANDOM);
    push_u64(g, &sp, 100); push_u64(g, &sp, AT_CLKTCK);

    /* HWCAP/HWCAP2: derived from VZ-sanitized ID register values */
    push_u64(g, &sp, query_hwcap()); push_u64(g, &sp, AT_HWCAP);
    push_u64(g, &sp, query_hwcap2()); push_u64(g, &sp, AT_HWCAP2);

    push_u64(g, &sp, 1000); push_u64(g, &sp, AT_EGID);
    push_u64(g, &sp, 1000); push_u64(g, &sp, AT_GID);
    push_u64(g, &sp, 1000); push_u64(g, &sp, AT_EUID);
    push_u64(g, &sp, 1000); push_u64(g, &sp, AT_UID);

    push_u64(g, &sp, elf_info->entry + elf_load_base); push_u64(g, &sp, AT_ENTRY);
    push_u64(g, &sp, elf_info->phnum); push_u64(g, &sp, AT_PHNUM);
    push_u64(g, &sp, elf_info->phentsize); push_u64(g, &sp, AT_PHENT);
    push_u64(g, &sp, elf_info->phdr_gpa + elf_load_base); push_u64(g, &sp, AT_PHDR);
    push_u64(g, &sp, 4096); push_u64(g, &sp, AT_PAGESZ);

    /* AT_SYSINFO_EHDR: vDSO ELF header address (required by rosetta) */
    if (vdso_base != 0) {
        push_u64(g, &sp, vdso_base); push_u64(g, &sp, AT_SYSINFO_EHDR);
    }

    /* AT_BASE: interpreter load base (only present for dynamic linking) */
    if (interp_base != 0) {
        push_u64(g, &sp, interp_base); push_u64(g, &sp, AT_BASE);
    }

    /* envp: environment variable pointers + NULL terminator */
    push_u64(g, &sp, 0);  /* NULL terminator */
    for (int i = envc - 1; i >= 0; i--) {
        push_u64(g, &sp, env_ptrs[i]);
    }

    /* argv: NULL terminator, then pointers in reverse order */
    push_u64(g, &sp, 0);  /* NULL terminator */
    for (int i = argc - 1; i >= 0; i--) {
        push_u64(g, &sp, arg_ptrs[i]);
    }

    /* argc — SP now points here, 16-byte aligned */
    push_u64(g, &sp, (uint64_t)argc);

    free(env_ptrs);
    free(arg_ptrs);
    return sp;
}
