/* test-rwx.c — Verify whether Apple HVF allows RWX page table entries
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone native macOS test program (NOT a guest binary). Uses HVF
 * directly to create a VM and test whether simultaneous read+write+execute
 * page table entries work at stage-1 when SCTLR_EL1.WXN=0.
 *
 * Background: hl creates RWX stage-1 page table entries for rosetta's
 * JIT buffer. The CLAUDE.md states "Apple HVF enforces W^X" but this
 * has never been empirically verified for the stage-1 case with WXN=0.
 *
 * Tests:
 *   1. RWX 2MB block — L2 block descriptor with AP=RW_EL0, UXN=0, PXN=0
 *   2. RWX 4KB page  — L3 page descriptor with the same RWX permissions
 *   3. Baseline RX   — Confirm execution works on a normal RX page
 *   4. Baseline RW   — Confirm writes work on a normal RW page
 *
 * For tests 1-2, the guest EL0 code (on a separate RX page):
 *   a. STR two ARM64 instructions (mov x0, #42; svc #0) to the RWX region
 *   b. BR to that address to execute the written code
 *   c. The SVC forwards via HVC #5 with x0=42
 *
 * If RWX works: host receives x0=42 via HVC #5 (success).
 * If W^X is enforced: either the STR faults (data abort) or the BR faults
 * (instruction abort permission fault), causing HVC #2 (bad exception).
 *
 * Build: clang -O2 -framework Hypervisor -arch arm64 -I_build -o $@ $<
 * Run:   codesign --entitlements entitlements.plist -f -s - $@ && ./$@
 */

#include <Hypervisor/Hypervisor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>

/* Embedded shim binary — same blob that hl uses */
#include "shim_blob.h"

/* ── Formatting ─────────────────────────────────────────────────── */

#define GREEN  "\033[0;32m"
#define RED    "\033[0;31m"
#define YELLOW "\033[1;33m"
#define CYAN   "\033[0;36m"
#define RESET  "\033[0m"

/* ── Page table descriptor bits ─────────────────────────────────── */

#define PT_VALID       (1ULL << 0)
#define PT_TABLE       (1ULL << 1)
#define PT_BLOCK       (1ULL << 0)   /* Valid bit for block descriptors */
#define PT_PAGE        (1ULL << 1)   /* L3 page descriptor bit */
#define PT_AF          (1ULL << 10)
#define PT_SH_ISH      (3ULL << 8)
#define PT_NS          (1ULL << 5)
#define PT_ATTR1       (1ULL << 2)   /* MAIR index 1: Normal WB cacheable */
#define PT_UXN         (1ULL << 54)
#define PT_PXN         (1ULL << 53)
#define PT_AP_RW_EL0   (1ULL << 6)   /* AP[2:1]=01 → RW at EL0 */
#define PT_AP_RO       (3ULL << 6)   /* AP[2:1]=11 → RO at EL0 */

#define PAGE_SIZE_4K   4096ULL
#define BLOCK_2MB      (2ULL * 1024 * 1024)

/* ── Memory layout (16MB total) ─────────────────────────────────── */

#define GUEST_SIZE     (16ULL * 1024 * 1024)

#define PT_POOL_BASE   0x00010000ULL  /* Page table pool start */
#define SHIM_BASE      0x00100000ULL  /* Shim code (RX) */
#define SHIM_DATA_BASE 0x00200000ULL  /* Shim data / EL1 stack (RW) */
#define GUEST_CODE     0x00400000ULL  /* EL0 test code (RX) */
#define RWX_BLOCK      0x00600000ULL  /* 2MB block for RWX test (test 1) */
#define RWX_PAGE_BLOCK 0x00800000ULL  /* 2MB region containing RWX 4KB page (test 2) */
#define GUEST_DATA     0x00A00000ULL  /* RW data (test 4 baseline) */
#define STACK_BASE     0x00C00000ULL  /* EL0 stack (RW) */

/* Within RWX_PAGE_BLOCK, the RWX 4KB page is at offset 0 */
#define RWX_PAGE_ADDR  RWX_PAGE_BLOCK

/* EL0 stack top and SP_EL1 */
#define SP_EL0         (STACK_BASE + BLOCK_2MB)
#define SP_EL1         (SHIM_DATA_BASE + BLOCK_2MB)

/* Code offsets within GUEST_CODE (4KB apart for different tests) */
#define CODE_TEST1     0x0000ULL     /* Test 1: RWX block write+exec */
#define CODE_TEST2     0x1000ULL     /* Test 2: RWX page write+exec */
#define CODE_TEST3     0x2000ULL     /* Test 3: baseline RX exec */
#define CODE_TEST4     0x3000ULL     /* Test 4: baseline RW write */

/* ── System register values (from hl.c) ────────────────────────── */

#define TCR_EL1_VALUE  0x5B5903510ULL

#define SCTLR_M    (1ULL << 0)
#define SCTLR_C    (1ULL << 2)
#define SCTLR_I    (1ULL << 12)
#define SCTLR_WXN  (1ULL << 19)
#define SCTLR_RES1 ((1ULL << 29) | (1ULL << 28) | (1ULL << 23) | \
                     (1ULL << 22) | (1ULL << 20) | (1ULL << 11) | \
                     (1ULL <<  8) | (1ULL <<  7))

/* WXN=0 explicitly — this is what we're testing */
#define SCTLR_NO_MMU  (SCTLR_RES1 | SCTLR_C | SCTLR_I)
#define SCTLR_WITH_MMU (SCTLR_RES1 | SCTLR_M | SCTLR_C | SCTLR_I)

/* ── Error handling ────────────────────────────────────────────── */

#define HV_CHECK(call) do {                                     \
    hv_return_t _r = (call);                                    \
    if (_r != HV_SUCCESS) {                                     \
        fprintf(stderr, "FAIL: %s returned %d\n", #call, (int)_r); \
        return -1;                                              \
    }                                                           \
} while (0)

/* ── VM state and page table allocator ─────────────────────────── */

typedef struct {
    void     *host_base;
    uint64_t  pt_next;
} vm_state_t;

static uint64_t pt_alloc(vm_state_t *vm) {
    if (vm->pt_next + PAGE_SIZE_4K > SHIM_BASE) {
        fprintf(stderr, "  PT pool exhausted\n");
        return 0;
    }
    uint64_t off = vm->pt_next;
    vm->pt_next += PAGE_SIZE_4K;
    memset((uint8_t *)vm->host_base + off, 0, PAGE_SIZE_4K);
    return off;
}

/* ── Descriptor builders ───────────────────────────────────────── */

/* Common base attributes for a 2MB block or 4KB page */
static uint64_t common_attrs(void) {
    return PT_AF | PT_SH_ISH | PT_NS | PT_ATTR1;
}

/* 2MB block: RX (executable, read-only at EL0) */
static uint64_t make_block_rx(uint64_t gpa) {
    return (gpa & 0xFFFFFFFFE00000ULL) | common_attrs()
         | PT_BLOCK | PT_AP_RO;
    /* UXN=0, PXN=0 → executable */
}

/* 2MB block: RW (writable, not executable) */
static uint64_t make_block_rw(uint64_t gpa) {
    return (gpa & 0xFFFFFFFFE00000ULL) | common_attrs()
         | PT_BLOCK | PT_AP_RW_EL0 | PT_UXN | PT_PXN;
}

/* 2MB block: RWX (writable AND executable at EL0 — the test subject) */
static uint64_t make_block_rwx(uint64_t gpa) {
    return (gpa & 0xFFFFFFFFE00000ULL) | common_attrs()
         | PT_BLOCK | PT_AP_RW_EL0;
    /* UXN=0, PXN=0 → executable; AP=01 → writable at EL0 */
}

/* 4KB L3 page: RWX (writable AND executable at EL0) */
static uint64_t make_page_rwx(uint64_t gpa) {
    return (gpa & 0xFFFFFFFFF000ULL) | common_attrs()
         | PT_VALID | PT_PAGE | PT_AP_RW_EL0;
    /* UXN=0, PXN=0 → executable; AP=01 → writable at EL0 */
}

/* 4KB L3 page: RW (not executable) */
static uint64_t make_page_rw(uint64_t gpa) {
    return (gpa & 0xFFFFFFFFF000ULL) | common_attrs()
         | PT_VALID | PT_PAGE | PT_AP_RW_EL0 | PT_UXN | PT_PXN;
}

/* ── Page table builder ────────────────────────────────────────── */

/* Layout used by all tests:
 *   L2[0]: 0x000000 (shim+PT pool, RX)
 *   L2[1]: 0x200000 (shim data/EL1 stack, RW)
 *   L2[2]: 0x400000 (guest EL0 code, RX)
 *   L2[3]: 0x600000 (RWX block test, RWX)       ← Test 1
 *   L2[4]: 0x800000 (table → L3 for RWX page)   ← Test 2
 *   L2[5]: 0xA00000 (guest data, RW)             ← Test 4
 *   L2[6]: 0xC00000 (stack, RW)
 *   L2[7]: 0xE00000 (stack top, RW)
 */
static uint64_t build_page_tables(vm_state_t *vm) {
    uint64_t l0_off = pt_alloc(vm);
    uint64_t l1_off = pt_alloc(vm);
    if (!l0_off || !l1_off) return 0;

    uint64_t *l0 = (uint64_t *)((uint8_t *)vm->host_base + l0_off);
    uint64_t *l1 = (uint64_t *)((uint8_t *)vm->host_base + l1_off);

    /* L0[0] → L1 table */
    l0[0] = l1_off | PT_VALID | PT_TABLE;

    /* L1[0] → L2 table */
    uint64_t l2_off = pt_alloc(vm);
    if (!l2_off) return 0;
    l1[0] = l2_off | PT_VALID | PT_TABLE;

    uint64_t *l2 = (uint64_t *)((uint8_t *)vm->host_base + l2_off);

    /* L2[0]: Shim code + PT pool (RX) */
    l2[0] = make_block_rx(0x000000);

    /* L2[1]: Shim data / EL1 stack (RW) */
    l2[1] = make_block_rw(0x200000);

    /* L2[2]: Guest EL0 code (RX) */
    l2[2] = make_block_rx(0x400000);

    /* L2[3]: RWX block — Test 1 subject */
    l2[3] = make_block_rwx(0x600000);

    /* L2[4]: Table descriptor → L3 page table for Test 2.
     * We split this 2MB block into 512 x 4KB pages. The first page
     * at 0x800000 is RWX, the rest are RW (non-executable). */
    {
        uint64_t l3_off = pt_alloc(vm);
        if (!l3_off) return 0;
        l2[4] = l3_off | PT_VALID | PT_TABLE;

        uint64_t *l3 = (uint64_t *)((uint8_t *)vm->host_base + l3_off);
        /* Page 0 (0x800000): RWX — the test subject */
        l3[0] = make_page_rwx(0x800000);
        /* Pages 1-511: RW (fill so address range is valid) */
        for (int i = 1; i < 512; i++)
            l3[i] = make_page_rw(0x800000 + (uint64_t)i * PAGE_SIZE_4K);
    }

    /* L2[5]: Guest data (RW, for test 4 baseline) */
    l2[5] = make_block_rw(0xA00000);

    /* L2[6]: Stack (RW) */
    l2[6] = make_block_rw(0xC00000);

    /* L2[7]: Stack top overflow (RW) */
    l2[7] = make_block_rw(0xE00000);

    return l0_off;
}

/* ── AArch64 instruction encoding helpers ──────────────────────── */

/* MOV Xd, #imm16 (MOVZ) */
static uint32_t movz(int rd, uint16_t imm) {
    return 0xD2800000 | ((uint32_t)imm << 5) | (uint32_t)rd;
}

/* MOV Xd, #imm16 shifted left 16 (MOVK hw=1) */
static uint32_t movk_lsl16(int rd, uint16_t imm) {
    return 0xF2A00000 | ((uint32_t)imm << 5) | (uint32_t)rd;
}

/* SVC #0 */
static uint32_t svc0(void) {
    return 0xD4000001;
}

/* STR Wt, [Xn, #imm12*4] (unsigned offset, 32-bit) */
static uint32_t str_w_imm(int rt, int rn, uint32_t byte_off) {
    uint32_t imm12 = byte_off / 4;
    return 0xB9000000 | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

/* BR Xn (branch to register) */
static uint32_t br(int rn) {
    return 0xD61F0000 | ((uint32_t)rn << 5);
}

/* STR Xt, [Xn, #imm12*8] (unsigned offset, 64-bit) */
static uint32_t str_imm64(int rt, int rn, uint32_t byte_off) {
    uint32_t imm12 = byte_off / 8;
    return 0xF9000000 | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

/* DSB ISH */
static uint32_t dsb_ish(void) {
    return 0xD5033F9F;  /* DSB ISH = 0xD5033F9F */
}

/* ISB */
static uint32_t isb(void) {
    return 0xD5033FDF;  /* ISB = 0xD5033FDF */
}

static void emit(void *base, uint64_t offset, const uint32_t *insns, int n) {
    memcpy((uint8_t *)base + offset, insns, (size_t)n * 4);
}

/* ── vCPU setup ────────────────────────────────────────────────── */

static int setup_vcpu(hv_vcpu_t vcpu, uint64_t ttbr0,
                      uint64_t entry_ipa, uint64_t sp_el0_val,
                      uint64_t sp_el1_val, uint64_t shim_ipa) {
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, shim_ipa + 0x800));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, 0xFF00));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, TCR_EL1_VALUE));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, ttbr0));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR1_EL1, 0));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, 3ULL << 20));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, entry_ipa));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, 0x0));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, sp_el0_val));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL1, sp_el1_val));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, SCTLR_NO_MMU));

    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, shim_ipa));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5)); /* EL1h */
    for (int i = 0; i < 31; i++)
        HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, 0));

    /* Pass SCTLR (with MMU, WXN=0) in X0 — shim enables MMU via HVC #4 */
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, SCTLR_WITH_MMU));

    return 0;
}

/* ── Exit reasons ──────────────────────────────────────────────── */

#define EXIT_HVC5    0  /* HVC #5: syscall forward */
#define EXIT_HVC0    1  /* HVC #0: normal exit */
#define EXIT_HVC2    2  /* HVC #2: bad exception */
#define EXIT_HVC9    9  /* HVC #9: W^X toggle request */
#define EXIT_ERROR   3  /* Unexpected exit */

typedef struct {
    int      reason;
    uint64_t x0, x1, x8;
    /* For HVC #2 (bad exception): ESR, FAR, ELR, SPSR, vector */
    uint64_t esr, far, elr, spsr, vec;
} vcpu_exit_t;

/* Decode ESR_EL1 exception class string */
static const char *ec_name(uint32_t ec) {
    switch (ec) {
    case 0x15: return "SVC (AArch64)";
    case 0x16: return "HVC (AArch64)";
    case 0x18: return "MSR/MRS trap";
    case 0x20: return "Instruction abort (lower EL)";
    case 0x21: return "Instruction abort (same EL)";
    case 0x24: return "Data abort (lower EL)";
    case 0x25: return "Data abort (same EL)";
    default:   return "unknown";
    }
}

/* Decode ISS fault status code for data/instruction aborts */
static const char *fsc_name(uint32_t fsc) {
    switch (fsc & 0x3F) {
    case 0x04: return "Translation fault L0";
    case 0x05: return "Translation fault L1";
    case 0x06: return "Translation fault L2";
    case 0x07: return "Translation fault L3";
    case 0x09: return "Access flag fault L1";
    case 0x0A: return "Access flag fault L2";
    case 0x0B: return "Access flag fault L3";
    case 0x0C: return "Permission fault L0";
    case 0x0D: return "Permission fault L1";
    case 0x0E: return "Permission fault L2";
    case 0x0F: return "Permission fault L3";
    default:   return "other";
    }
}

/* Run the vCPU until exit. Handles HVC #4 (sysreg set) internally.
 * Returns 0 on clean exit (HVC #0, #5, or #2), -1 on error.
 * For HVC #9 (W^X toggle), returns immediately so caller can decide. */
static int run_vcpu_once(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit,
                         vcpu_exit_t *out) {
    for (int iter = 0; iter < 10000; iter++) {
        hv_return_t ret = hv_vcpu_run(vcpu);
        if (ret != HV_SUCCESS) {
            fprintf(stderr, "  hv_vcpu_run failed: %d\n", (int)ret);
            out->reason = EXIT_ERROR;
            return -1;
        }

        if (vexit->reason == HV_EXIT_REASON_EXCEPTION) {
            uint32_t ec = (vexit->exception.syndrome >> 26) & 0x3F;

            if (ec == 0x16) {
                /* HVC */
                uint16_t imm = vexit->exception.syndrome & 0xFFFF;

                if (imm == 4) {
                    /* HVC #4: set sysreg (from shim _start) */
                    uint64_t reg_id, value;
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &reg_id);
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &value);

                    hv_sys_reg_t hv_reg;
                    switch ((int)reg_id) {
                    case 0: hv_reg = HV_SYS_REG_VBAR_EL1; break;
                    case 1: hv_reg = HV_SYS_REG_MAIR_EL1; break;
                    case 2: hv_reg = HV_SYS_REG_TCR_EL1;  break;
                    case 3: hv_reg = HV_SYS_REG_TTBR0_EL1; break;
                    case 4: hv_reg = HV_SYS_REG_SCTLR_EL1; break;
                    case 5: hv_reg = HV_SYS_REG_CPACR_EL1; break;
                    default:
                        fprintf(stderr, "  HVC #4 unknown reg %llu\n",
                                (unsigned long long)reg_id);
                        out->reason = EXIT_ERROR;
                        return -1;
                    }
                    hv_vcpu_set_sys_reg(vcpu, hv_reg, value);
                    continue;

                } else if (imm == 5) {
                    /* HVC #5: syscall forward */
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &out->x0);
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &out->x1);
                    hv_vcpu_get_reg(vcpu, HV_REG_X8, &out->x8);
                    out->reason = EXIT_HVC5;
                    return 0;

                } else if (imm == 0) {
                    /* HVC #0: normal exit */
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &out->x0);
                    out->reason = EXIT_HVC0;
                    return 0;

                } else if (imm == 2) {
                    /* HVC #2: bad exception */
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &out->esr);
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &out->far);
                    hv_vcpu_get_reg(vcpu, HV_REG_X2, &out->elr);
                    hv_vcpu_get_reg(vcpu, HV_REG_X3, &out->spsr);
                    hv_vcpu_get_reg(vcpu, HV_REG_X5, &out->vec);
                    out->reason = EXIT_HVC2;
                    return 0;

                } else if (imm == 9) {
                    /* HVC #9: W^X toggle request */
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &out->x0);  /* FAR */
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &out->x1);  /* type */
                    out->reason = EXIT_HVC9;
                    return 0;
                }
            }
        }

        /* Unexpected exit */
        fprintf(stderr, "  unexpected exit reason=0x%x syndrome=0x%llx\n",
                vexit->reason,
                (unsigned long long)vexit->exception.syndrome);
        out->reason = EXIT_ERROR;
        return -1;
    }

    fprintf(stderr, "  exceeded max iterations\n");
    out->reason = EXIT_ERROR;
    return -1;
}

/* ── VM lifecycle ──────────────────────────────────────────────── */

static int vm_create(vm_state_t *vm) {
    vm->host_base = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                         MAP_ANON | MAP_PRIVATE, -1, 0);
    if (vm->host_base == MAP_FAILED) return -1;
    vm->pt_next = PT_POOL_BASE;

    uint32_t max_ipa = 0;
    hv_vm_config_get_max_ipa_size(&max_ipa);
    if (max_ipa >= 40) max_ipa = 40;
    else max_ipa = 36;

    hv_vm_config_t config = hv_vm_config_create();
    hv_vm_config_set_ipa_size(config, max_ipa);
    hv_return_t ret = hv_vm_create(config);
    os_release(config);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "  hv_vm_create failed: %d\n", (int)ret);
        munmap(vm->host_base, GUEST_SIZE);
        return -1;
    }

    /* Map guest memory with full RWX at stage-2 (HVF level).
     * Stage-1 (page table) permissions are what we're testing. */
    ret = hv_vm_map(vm->host_base, 0, GUEST_SIZE,
                    HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "  hv_vm_map failed: %d\n", (int)ret);
        hv_vm_destroy();
        munmap(vm->host_base, GUEST_SIZE);
        return -1;
    }

    /* Load shim binary */
    memcpy((uint8_t *)vm->host_base + SHIM_BASE, shim_bin, shim_bin_len);

    return 0;
}

static void vm_destroy(vm_state_t *vm) {
    hv_vm_destroy();
    munmap(vm->host_base, GUEST_SIZE);
}

/* ── Print bad exception details ───────────────────────────────── */

static void print_bad_exception(const vcpu_exit_t *ex) {
    uint32_t ec = (uint32_t)(ex->esr >> 26) & 0x3F;
    uint32_t fsc = (uint32_t)(ex->esr & 0x3F);
    fprintf(stderr, "  bad exception at vec=0x%03llx:\n",
            (unsigned long long)ex->vec);
    fprintf(stderr, "    ESR=0x%08llx  EC=0x%02x (%s)\n",
            (unsigned long long)ex->esr, ec, ec_name(ec));
    fprintf(stderr, "    FAR=0x%llx  ELR=0x%llx  SPSR=0x%llx\n",
            (unsigned long long)ex->far,
            (unsigned long long)ex->elr,
            (unsigned long long)ex->spsr);
    if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) {
        fprintf(stderr, "    FSC=0x%02x (%s)\n", fsc, fsc_name(fsc));
        if (ec == 0x24 || ec == 0x25)
            fprintf(stderr, "    WnR=%d\n", (int)((ex->esr >> 6) & 1));
    }
}

/* ════════════════════════════════════════════════════════════════════
 * TEST 1: RWX 2MB Block
 *
 * Stage-1 page table has a 2MB block at 0x600000 with:
 *   AP[2:1]=01 (RW at EL0), UXN=0, PXN=0 (executable)
 * This is a true RWX mapping.
 *
 * Guest EL0 code (on separate RX page at 0x400000):
 *   1. Write "mov x0, #42" and "svc #0" to 0x600000 using STR
 *   2. DSB ISH + ISB for data/instruction cache coherence
 *   3. BR to 0x600000
 *
 * If successful: HVC #5 with x0=42
 * If W^X enforced on write: data abort permission fault on STR
 * If W^X enforced on exec: instruction abort permission fault on BR
 * ════════════════════════════════════════════════════════════════════ */

static int test1_rwx_block(void) {
    vm_state_t vm;
    if (vm_create(&vm) < 0) return -1;

    uint64_t ttbr0 = build_page_tables(&vm);
    if (!ttbr0) { vm_destroy(&vm); return -1; }

    /* Guest EL0 code: self-modifying code test
     *
     * The instructions we want to write to the RWX page:
     *   mov x0, #42     = 0xD2800540
     *   svc #0          = 0xD4000001
     *
     * Guest code sequence:
     *   mov x1, #0x0054         ; low 16 bits of 0xD2800540
     *   movk x1, #0xD280, lsl#16 ; "mov x0, #42" encoding
     *   mov x2, <RWX_BLOCK lo>
     *   movk x2, <RWX_BLOCK hi>
     *   str w1, [x2, #0]        ; write "mov x0, #42" to RWX page
     *   mov x1, #0x0001         ; low 16 bits of 0xD4000001
     *   movk x1, #0xD400, lsl#16 ; "svc #0" encoding
     *   str w1, [x2, #4]        ; write "svc #0" to RWX page + 4
     *   dsb ish                  ; data barrier (ensure stores complete)
     *   isb                      ; instruction barrier
     *   br x2                    ; branch to the written code
     */
    {
        uint32_t mov_x0_42 = movz(0, 42);     /* 0xD2800540 */
        uint32_t svc_0     = svc0();           /* 0xD4000001 */

        uint32_t code[] = {
            /* Load "mov x0, #42" encoding into x1 */
            movz(1, (uint16_t)(mov_x0_42 & 0xFFFF)),
            movk_lsl16(1, (uint16_t)((mov_x0_42 >> 16) & 0xFFFF)),

            /* Load RWX_BLOCK address into x2 */
            movz(2, (uint16_t)(RWX_BLOCK & 0xFFFF)),
            movk_lsl16(2, (uint16_t)((RWX_BLOCK >> 16) & 0xFFFF)),

            /* Write "mov x0, #42" to RWX page */
            str_w_imm(1, 2, 0),

            /* Load "svc #0" encoding into x1 */
            movz(1, (uint16_t)(svc_0 & 0xFFFF)),
            movk_lsl16(1, (uint16_t)((svc_0 >> 16) & 0xFFFF)),

            /* Write "svc #0" to RWX page + 4 */
            str_w_imm(1, 2, 4),

            /* Memory barriers for I/D cache coherence */
            dsb_ish(),
            isb(),

            /* Branch to the written code */
            br(2),
        };
        emit(vm.host_base, GUEST_CODE + CODE_TEST1, code, 11);
    }

    /* Create vCPU and run */
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    hv_return_t ret = hv_vcpu_create(&vcpu, &vexit, NULL);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "  hv_vcpu_create failed: %d\n", (int)ret);
        vm_destroy(&vm);
        return -1;
    }

    if (setup_vcpu(vcpu, ttbr0, GUEST_CODE + CODE_TEST1,
                   SP_EL0, SP_EL1, SHIM_BASE) < 0) {
        hv_vcpu_destroy(vcpu);
        vm_destroy(&vm);
        return -1;
    }

    vcpu_exit_t ex;
    run_vcpu_once(vcpu, vexit, &ex);

    int result = -1;

    if (ex.reason == EXIT_HVC9) {
        /* W^X toggle request — the shim detected a permission fault
         * and is asking host to flip permissions. This means HVF DOES
         * enforce W^X even with WXN=0 in SCTLR_EL1. */
        printf("\n");
        printf("    W^X toggle requested: FAR=0x%llx type=%llu (%s)\n",
               (unsigned long long)ex.x0,
               (unsigned long long)ex.x1,
               ex.x1 == 0 ? "exec fault -> flip to RX" : "write fault -> flip to RW");
        printf("    " YELLOW "HVF enforces W^X at stage-2 despite WXN=0" RESET "\n");
        result = -1;

    } else if (ex.reason == EXIT_HVC5) {
        /* Syscall forward — the written code executed! */
        if (ex.x0 == 42) {
            printf("\n    " GREEN "RWX works!" RESET
                   " Written code executed successfully (x0=%llu)\n",
                   (unsigned long long)ex.x0);
            result = 0;
        } else {
            printf("\n    Unexpected x0=%llu (expected 42)\n",
                   (unsigned long long)ex.x0);
            result = -1;
        }

    } else if (ex.reason == EXIT_HVC2) {
        /* Bad exception — W^X enforced */
        printf("\n");
        print_bad_exception(&ex);
        uint32_t ec = (uint32_t)(ex.esr >> 26) & 0x3F;
        if (ec == 0x20) {
            printf("    " YELLOW "Instruction abort: W^X blocks execution on writable page" RESET "\n");
        } else if (ec == 0x24) {
            printf("    " YELLOW "Data abort: W^X blocks write to executable page" RESET "\n");
        }
        result = -1;

    } else {
        printf("\n    Unexpected exit reason=%d\n", ex.reason);
        result = -1;
    }

    hv_vcpu_destroy(vcpu);
    vm_destroy(&vm);
    return result;
}

/* ════════════════════════════════════════════════════════════════════
 * TEST 2: RWX 4KB Page (L3 descriptor)
 *
 * Same as test 1, but using a 4KB L3 page descriptor at 0x800000
 * instead of a 2MB L2 block descriptor. Tests whether the
 * granularity matters for W^X enforcement.
 * ════════════════════════════════════════════════════════════════════ */

static int test2_rwx_page(void) {
    vm_state_t vm;
    if (vm_create(&vm) < 0) return -1;

    uint64_t ttbr0 = build_page_tables(&vm);
    if (!ttbr0) { vm_destroy(&vm); return -1; }

    /* Same code as test 1, but targeting RWX_PAGE_ADDR (0x800000) */
    {
        uint32_t mov_x0_42 = movz(0, 42);
        uint32_t svc_0     = svc0();

        uint32_t code[] = {
            movz(1, (uint16_t)(mov_x0_42 & 0xFFFF)),
            movk_lsl16(1, (uint16_t)((mov_x0_42 >> 16) & 0xFFFF)),

            movz(2, (uint16_t)(RWX_PAGE_ADDR & 0xFFFF)),
            movk_lsl16(2, (uint16_t)((RWX_PAGE_ADDR >> 16) & 0xFFFF)),

            str_w_imm(1, 2, 0),

            movz(1, (uint16_t)(svc_0 & 0xFFFF)),
            movk_lsl16(1, (uint16_t)((svc_0 >> 16) & 0xFFFF)),

            str_w_imm(1, 2, 4),

            dsb_ish(),
            isb(),

            br(2),
        };
        emit(vm.host_base, GUEST_CODE + CODE_TEST2, code, 11);
    }

    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    hv_return_t ret = hv_vcpu_create(&vcpu, &vexit, NULL);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "  hv_vcpu_create failed: %d\n", (int)ret);
        vm_destroy(&vm);
        return -1;
    }

    if (setup_vcpu(vcpu, ttbr0, GUEST_CODE + CODE_TEST2,
                   SP_EL0, SP_EL1, SHIM_BASE) < 0) {
        hv_vcpu_destroy(vcpu);
        vm_destroy(&vm);
        return -1;
    }

    vcpu_exit_t ex;
    run_vcpu_once(vcpu, vexit, &ex);

    int result = -1;

    if (ex.reason == EXIT_HVC9) {
        printf("\n");
        printf("    W^X toggle requested: FAR=0x%llx type=%llu (%s)\n",
               (unsigned long long)ex.x0,
               (unsigned long long)ex.x1,
               ex.x1 == 0 ? "exec fault -> flip to RX" : "write fault -> flip to RW");
        printf("    " YELLOW "HVF enforces W^X at stage-2 (4KB page)" RESET "\n");
        result = -1;

    } else if (ex.reason == EXIT_HVC5 && ex.x0 == 42) {
        printf("\n    " GREEN "RWX works!" RESET
               " Written code executed (4KB page, x0=%llu)\n",
               (unsigned long long)ex.x0);
        result = 0;

    } else if (ex.reason == EXIT_HVC2) {
        printf("\n");
        print_bad_exception(&ex);
        uint32_t ec = (uint32_t)(ex.esr >> 26) & 0x3F;
        if (ec == 0x20)
            printf("    " YELLOW "Instruction abort: W^X blocks execution (4KB page)" RESET "\n");
        else if (ec == 0x24)
            printf("    " YELLOW "Data abort: W^X blocks write (4KB page)" RESET "\n");
        result = -1;

    } else {
        printf("\n    Unexpected: reason=%d x0=%llu\n",
               ex.reason, (unsigned long long)ex.x0);
        result = -1;
    }

    hv_vcpu_destroy(vcpu);
    vm_destroy(&vm);
    return result;
}

/* ════════════════════════════════════════════════════════════════════
 * TEST 3: Baseline RX (execution on read-only page)
 *
 * Verify that normal RX execution works. Guest code at 0x400000
 * (GUEST_CODE) is on an RX page. This test just runs code that
 * does "mov x0, #99; svc #0" to confirm the setup is correct.
 * ════════════════════════════════════════════════════════════════════ */

static int test3_baseline_rx(void) {
    vm_state_t vm;
    if (vm_create(&vm) < 0) return -1;

    uint64_t ttbr0 = build_page_tables(&vm);
    if (!ttbr0) { vm_destroy(&vm); return -1; }

    /* Simple code: mov x0, #99; mov x8, #0; svc #0 */
    uint32_t code[] = {
        movz(0, 99),
        movz(8, 0),  /* syscall nr doesn't matter, we just check x0 */
        svc0()
    };
    emit(vm.host_base, GUEST_CODE + CODE_TEST3, code, 3);

    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    hv_return_t ret = hv_vcpu_create(&vcpu, &vexit, NULL);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "  hv_vcpu_create failed: %d\n", (int)ret);
        vm_destroy(&vm);
        return -1;
    }

    if (setup_vcpu(vcpu, ttbr0, GUEST_CODE + CODE_TEST3,
                   SP_EL0, SP_EL1, SHIM_BASE) < 0) {
        hv_vcpu_destroy(vcpu);
        vm_destroy(&vm);
        return -1;
    }

    vcpu_exit_t ex;
    run_vcpu_once(vcpu, vexit, &ex);

    int result = -1;
    if (ex.reason == EXIT_HVC5 && ex.x0 == 99) {
        result = 0;
    } else if (ex.reason == EXIT_HVC2) {
        print_bad_exception(&ex);
        result = -1;
    } else {
        printf("\n    Unexpected: reason=%d x0=%llu\n",
               ex.reason, (unsigned long long)ex.x0);
        result = -1;
    }

    hv_vcpu_destroy(vcpu);
    vm_destroy(&vm);
    return result;
}

/* ════════════════════════════════════════════════════════════════════
 * TEST 4: Baseline RW (write to writable page)
 *
 * Verify that normal RW writes work. Guest writes a value to the
 * RW data page at 0xA00000, then reports it via SVC.
 * ════════════════════════════════════════════════════════════════════ */

static int test4_baseline_rw(void) {
    vm_state_t vm;
    if (vm_create(&vm) < 0) return -1;

    uint64_t ttbr0 = build_page_tables(&vm);
    if (!ttbr0) { vm_destroy(&vm); return -1; }

    /* Guest code: write 0x77 to GUEST_DATA, read it back, report via SVC
     *   mov x0, #0x77
     *   mov x1, <GUEST_DATA>
     *   str x0, [x1]
     *   ldr x0, [x1]       ; read back
     *   mov x8, #0
     *   svc #0
     */
    {
        uint32_t ldr_x0_x1 = 0xF9400020; /* ldr x0, [x1, #0] */
        uint32_t code[] = {
            movz(0, 0x77),
            movz(1, (uint16_t)(GUEST_DATA & 0xFFFF)),
            movk_lsl16(1, (uint16_t)((GUEST_DATA >> 16) & 0xFFFF)),
            str_imm64(0, 1, 0),
            ldr_x0_x1,
            movz(8, 0),
            svc0()
        };
        emit(vm.host_base, GUEST_CODE + CODE_TEST4, code, 7);
    }

    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    hv_return_t ret = hv_vcpu_create(&vcpu, &vexit, NULL);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "  hv_vcpu_create failed: %d\n", (int)ret);
        vm_destroy(&vm);
        return -1;
    }

    if (setup_vcpu(vcpu, ttbr0, GUEST_CODE + CODE_TEST4,
                   SP_EL0, SP_EL1, SHIM_BASE) < 0) {
        hv_vcpu_destroy(vcpu);
        vm_destroy(&vm);
        return -1;
    }

    vcpu_exit_t ex;
    run_vcpu_once(vcpu, vexit, &ex);

    int result = -1;
    if (ex.reason == EXIT_HVC5 && ex.x0 == 0x77) {
        result = 0;
    } else if (ex.reason == EXIT_HVC2) {
        print_bad_exception(&ex);
        result = -1;
    } else {
        printf("\n    Unexpected: reason=%d x0=%llu\n",
               ex.reason, (unsigned long long)ex.x0);
        result = -1;
    }

    hv_vcpu_destroy(vcpu);
    vm_destroy(&vm);
    return result;
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(void) {
    printf("RWX Page Table Entry Test for Apple Hypervisor.framework\n");
    printf("=========================================================\n\n");
    printf("Question: Does HVF allow simultaneous RWX page table entries\n"
           "at stage-1 (guest page tables) when SCTLR_EL1.WXN=0?\n\n");

    printf(CYAN "SCTLR_EL1 = 0x%llx (WXN=%s)\n" RESET,
           (unsigned long long)SCTLR_WITH_MMU,
           (SCTLR_WITH_MMU & SCTLR_WXN) ? "1" : "0");
    printf(CYAN "Stage-2: hv_vm_map with READ|WRITE|EXEC\n\n" RESET);

    int pass = 0, fail = 0;

    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "Baseline: RX execution",           test3_baseline_rx },
        { "Baseline: RW write",               test4_baseline_rw },
        { "RWX 2MB block (write+exec)",        test1_rwx_block },
        { "RWX 4KB page  (write+exec)",        test2_rwx_page },
    };
    int ntests = (int)(sizeof(tests) / sizeof(tests[0]));

    for (int i = 0; i < ntests; i++) {
        printf("TEST %d: %-40s ", i + 1, tests[i].name);
        fflush(stdout);

        int rc = tests[i].fn();
        if (rc == 0) {
            printf(GREEN "PASS" RESET "\n");
            pass++;
        } else {
            printf(RED "FAIL" RESET "\n");
            fail++;
        }
    }

    printf("\nResults: %d/%d passed\n\n", pass, ntests);

    /* Summary */
    if (pass == ntests) {
        printf(GREEN "CONCLUSION: Apple HVF allows RWX page table entries at stage-1\n"
               "when SCTLR_EL1.WXN=0. Self-modifying code works without W^X toggling.\n" RESET);
    } else if (pass >= 2 && fail > 0) {
        printf(YELLOW "CONCLUSION: Apple HVF enforces W^X at stage-2 (hypervisor level)\n"
               "regardless of stage-1 page table permissions and SCTLR.WXN.\n"
               "RWX page table entries are silently restricted. The W^X demand-toggle\n"
               "mechanism (HVC #9) in the shim is REQUIRED for JIT use cases.\n" RESET);
    } else {
        printf(RED "CONCLUSION: Unexpected results — check test output above.\n" RESET);
    }

    return fail > 0 ? 1 : 0;
}
