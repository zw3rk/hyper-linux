/* test-multi-vcpu.c — Validate multi-vCPU support in Apple Hypervisor.framework
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone native macOS test program (NOT a guest binary). Uses HVF
 * directly to create a VM with two vCPUs sharing the same guest physical
 * memory, page tables, and shim binary. Validates the fundamentals needed
 * for Linux clone(CLONE_THREAD) support in hl.
 *
 * Tests:
 *   1. Basic dual vCPU creation — can hv_vcpu_create() be called twice?
 *   2. Shared memory writes — can two vCPUs write to the same guest RAM?
 *   3. Separate SP_EL1 stacks — do per-vCPU EL1 stacks work with SVCs?
 *   4. Register preservation — are callee-saved regs safe across SVCs?
 *   5. TLBI broadcast — does TLBI VMALLE1IS propagate across vCPUs?
 *
 * Build: clang -O2 -framework Hypervisor -arch arm64 -I_build -o test $<
 * Run:   codesign --entitlements entitlements.plist -f -s - test && ./test
 */

#include <Hypervisor/Hypervisor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

/* Embedded shim binary — same blob that hl uses */
#include "shim_blob.h"

/* ── Formatting ─────────────────────────────────────────────────── */

#define GREEN  "\033[0;32m"
#define RED    "\033[0;31m"
#define YELLOW "\033[1;33m"
#define RESET  "\033[0m"

/* ── Page table descriptor bits (from guest.c) ──────────────────── */

#define PT_VALID       (1ULL << 0)
#define PT_TABLE       (1ULL << 1)
#define PT_BLOCK       (1ULL << 0)   /* Valid bit only for block descriptors */
#define PT_AF          (1ULL << 10)
#define PT_SH_ISH      (3ULL << 8)
#define PT_NS          (1ULL << 5)
#define PT_ATTR1       (1ULL << 2)   /* MAIR index 1: Normal WB cacheable */
#define PT_UXN         (1ULL << 54)
#define PT_PXN         (1ULL << 53)
#define PT_AP_RW_EL0   (1ULL << 6)   /* RW at EL0 */
#define PT_AP_RO       (3ULL << 6)   /* RO at EL0 */

#define PAGE_SIZE_4K   4096ULL
#define BLOCK_2MB      (2ULL * 1024 * 1024)
#define BLOCK_1GB      (1ULL * 1024 * 1024 * 1024)

/* ── Memory layout (16MB total, much smaller than hl's 32GB) ───── */

#define GUEST_SIZE     (16ULL * 1024 * 1024)

#define PT_POOL_BASE   0x00010000ULL  /* Page table pool start */
#define SHIM_BASE      0x00100000ULL  /* Shim code (RX) */
#define SHIM_DATA_BASE 0x00200000ULL  /* Shim data / EL1 stacks (RW) */
#define GUEST_CODE     0x00400000ULL  /* Guest EL0 code (RX) */
#define GUEST_DATA     0x00600000ULL  /* Guest shared data (RW) */
#define TLBI_REGION    0x00800000ULL  /* Initially unmapped (Test 5) */
#define STACK_A_BASE   0x00A00000ULL  /* EL0 stack A (RW) */
#define STACK_B_BASE   0x00C00000ULL  /* EL0 stack B (RW) */

/* vCPU-A and vCPU-B SP_EL1 (top of respective 512KB regions within shim data) */
#define SP_EL1_A       (SHIM_DATA_BASE + BLOCK_2MB)      /* 0x400000 */
#define SP_EL1_B       (SHIM_DATA_BASE + BLOCK_2MB / 2)  /* 0x300000 */

/* vCPU-A and vCPU-B EL0 code offsets within GUEST_CODE region */
#define CODE_A_OFF     0x0000ULL
#define CODE_B_OFF     0x1000ULL  /* 4KB apart */

/* EL0 stack tops (top of each 2MB region) */
#define SP_EL0_A       (STACK_A_BASE + BLOCK_2MB)  /* 0xC00000 */
#define SP_EL0_B       (STACK_B_BASE + BLOCK_2MB)  /* 0xE00000 */

/* ── System register values (from hl.c) ─────────────────────────── */

#define TCR_EL1_VALUE  0x5B5903510ULL

#define SCTLR_M   (1ULL << 0)
#define SCTLR_C   (1ULL << 2)
#define SCTLR_I   (1ULL << 12)
#define SCTLR_RES1 ((1ULL << 29) | (1ULL << 28) | (1ULL << 23) | \
                     (1ULL << 22) | (1ULL << 20) | (1ULL << 11) | \
                     (1ULL <<  8) | (1ULL <<  7))

#define SCTLR_NO_MMU  (SCTLR_RES1 | SCTLR_C | SCTLR_I)
#define SCTLR_WITH_MMU (SCTLR_RES1 | SCTLR_M | SCTLR_C | SCTLR_I)

/* Permission flags */
#define PERM_RX  1
#define PERM_RW  2

/* ── Error handling ─────────────────────────────────────────────── */

#define HV_CHECK(call) do {                                     \
    hv_return_t _r = (call);                                    \
    if (_r != HV_SUCCESS) {                                     \
        fprintf(stderr, "FAIL: %s returned %d\n", #call, (int)_r); \
        return -1;                                              \
    }                                                           \
} while (0)

/* ── Page table builder (simplified from guest.c) ───────────────── */

/* Simple page table pool allocator — one per test since each test
 * creates a fresh VM. */
typedef struct {
    void     *host_base;  /* Host mmap pointer */
    uint64_t  pt_next;    /* Next free offset in PT pool */
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

/* Build a 2MB block descriptor at a given GPA with RX or RW perms. */
static uint64_t make_block(uint64_t gpa, int perm) {
    uint64_t desc = (gpa & 0xFFFFFFFFE00000ULL)
                  | PT_AF | PT_SH_ISH | PT_NS | PT_ATTR1 | PT_BLOCK;
    if (perm == PERM_RX) {
        /* Executable, read-only */
        desc |= PT_AP_RO;
        /* PXN=0, UXN=0 → executable at both EL0 and EL1 */
    } else {
        /* RW, not executable */
        desc |= PT_AP_RW_EL0 | PT_UXN | PT_PXN;
    }
    return desc;
}

/* Build 3-level page tables mapping our memory layout.
 * Returns TTBR0 value (GPA of L0 table), or 0 on failure. */
static uint64_t build_page_tables(vm_state_t *vm, int include_tlbi_region) {
    uint64_t l0_off = pt_alloc(vm);
    uint64_t l1_off = pt_alloc(vm);
    if (!l0_off || !l1_off) return 0;

    uint64_t *l0 = (uint64_t *)((uint8_t *)vm->host_base + l0_off);
    uint64_t *l1 = (uint64_t *)((uint8_t *)vm->host_base + l1_off);

    /* L0[0] → L1 table (all our addresses are < 512GB) */
    l0[0] = l1_off | PT_VALID | PT_TABLE;

    /* L1[0] → L2 table (all our addresses are < 1GB) */
    uint64_t l2_off = pt_alloc(vm);
    if (!l2_off) return 0;
    l1[0] = l2_off | PT_VALID | PT_TABLE;

    uint64_t *l2 = (uint64_t *)((uint8_t *)vm->host_base + l2_off);

    /* Map 2MB blocks. L2 index = addr / 2MB. */
    /* Shim code (RX) at 0x100000 → L2[0] (shares 0x0-0x1FFFFF) */
    l2[0] = make_block(0x000000, PERM_RX);

    /* Shim data/stacks (RW) at 0x200000 → L2[1] */
    l2[1] = make_block(0x200000, PERM_RW);

    /* Guest EL0 code (RX) at 0x400000 → L2[2] */
    l2[2] = make_block(0x400000, PERM_RX);

    /* Guest data (RW) at 0x600000 → L2[3] */
    l2[3] = make_block(0x600000, PERM_RW);

    /* TLBI test region at 0x800000 → L2[4]: conditionally mapped */
    if (include_tlbi_region)
        l2[4] = make_block(0x800000, PERM_RW);
    /* else: L2[4] stays 0 → unmapped */

    /* EL0 stack A (RW) at 0xA00000 → L2[5] */
    l2[5] = make_block(0xA00000, PERM_RW);

    /* EL0 stack B (RW) at 0xC00000 → L2[6] */
    l2[6] = make_block(0xC00000, PERM_RW);

    /* Stack B spills into 0xE00000 (SP=0xE00000 grows down into 0xC00000 block)
     * — already covered by L2[6] since SP_EL0_B = 0xE00000 is top of 0xC00000 block.
     * Actually 0xE00000 = 7 * 2MB, that's a separate block. Map it too: */
    l2[7] = make_block(0xE00000, PERM_RW);

    return l0_off;
}

/* Add a single L2 entry for the TLBI test region (called at runtime). */
static int add_tlbi_mapping(vm_state_t *vm, uint64_t ttbr0) {
    uint64_t *l0 = (uint64_t *)((uint8_t *)vm->host_base + ttbr0);
    uint64_t l1_off = l0[0] & 0xFFFFFFFFF000ULL;
    uint64_t *l1 = (uint64_t *)((uint8_t *)vm->host_base + l1_off);
    uint64_t l2_off = l1[0] & 0xFFFFFFFFF000ULL;
    uint64_t *l2 = (uint64_t *)((uint8_t *)vm->host_base + l2_off);

    /* L2[4] = 0x800000 (TLBI_REGION) */
    l2[4] = make_block(0x800000, PERM_RW);
    return 0;
}

/* ── AArch64 instruction encoding helpers ───────────────────────── */

/* MOV Xd, #imm16 (MOVZ) */
static uint32_t movz(int rd, uint16_t imm) {
    return 0xD2800000 | ((uint32_t)imm << 5) | (uint32_t)rd;
}

/* MOV Xd, #imm16 shifted left by 16 (MOVK hw=1) */
static uint32_t movk_lsl16(int rd, uint16_t imm) {
    return 0xF2A00000 | ((uint32_t)imm << 5) | (uint32_t)rd;
}

/* SVC #0 */
static uint32_t svc0(void) {
    return 0xD4000001;
}

/* STR Xt, [Xn, #imm12*8] (unsigned offset, 64-bit) */
static uint32_t str_imm(int rt, int rn, uint32_t byte_off) {
    /* Encoding: 0xF9000000 | (imm12 << 10) | (rn << 5) | rt
     * where imm12 = byte_off / 8 */
    uint32_t imm12 = byte_off / 8;
    return 0xF9000000 | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

/* LDR Xt, [Xn, #imm12*8] (unsigned offset, 64-bit) */
static uint32_t ldr_imm(int rt, int rn, uint32_t byte_off) {
    uint32_t imm12 = byte_off / 8;
    return 0xF9400000 | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}

/* MOV Xd, Xm (ORR Xd, XZR, Xm) */
static uint32_t mov_reg(int rd, int rm) {
    return 0xAA0003E0 | ((uint32_t)rm << 16) | (uint32_t)rd;
}

/* ── Place guest code into host memory ──────────────────────────── */

static void emit(void *base, uint64_t offset, const uint32_t *insns, int n) {
    memcpy((uint8_t *)base + offset, insns, (size_t)n * 4);
}

/* ── vCPU setup (replicates hl.c:450-527) ───────────────────────── */

static int setup_vcpu(hv_vcpu_t vcpu, uint64_t ttbr0,
                      uint64_t entry_ipa, uint64_t sp_el0,
                      uint64_t sp_el1, uint64_t shim_ipa) {
    /* System registers */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, shim_ipa + 0x800));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, 0xFF00));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, TCR_EL1_VALUE));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, ttbr0));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR1_EL1, 0));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, 3ULL << 20));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, entry_ipa));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, 0x0));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, sp_el0));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL1, sp_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, SCTLR_NO_MMU));

    /* General purpose registers */
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, shim_ipa));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5)); /* EL1h */
    for (int i = 0; i < 31; i++)
        HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, 0));

    /* Pass SCTLR (with MMU) in X0 — shim enables MMU via HVC #4 */
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, SCTLR_WITH_MMU));

    return 0;
}

/* ── Minimal vCPU run loop ──────────────────────────────────────── */

/* Exit reasons from the run loop */
#define EXIT_HVC5    0  /* HVC #5: syscall forward (X8=syscall nr) */
#define EXIT_HVC0    1  /* HVC #0: normal exit */
#define EXIT_HVC2    2  /* HVC #2: bad exception */
#define EXIT_ERROR   3  /* Unexpected exit */

typedef struct {
    int      reason;
    uint64_t x0, x1, x8;  /* Registers at exit */
} vcpu_exit_t;

/* Run the vCPU until it exits via HVC #0 or HVC #5. Handles HVC #4
 * (sysreg set) internally, same as hl's run loop. Returns the exit
 * reason and register values. max_hvc5 limits how many HVC #5 exits
 * to handle before returning (0 = stop on first HVC #5). */
static int run_vcpu(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit,
                    vcpu_exit_t *out, int max_hvc5) {
    int hvc5_count = 0;

    for (int iter = 0; iter < 10000; iter++) {
        HV_CHECK(hv_vcpu_run(vcpu));

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
                    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, hv_reg, value));
                    continue;

                } else if (imm == 5) {
                    /* HVC #5: syscall forward */
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &out->x0);
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &out->x1);
                    hv_vcpu_get_reg(vcpu, HV_REG_X8, &out->x8);
                    out->reason = EXIT_HVC5;

                    if (hvc5_count < max_hvc5) {
                        hvc5_count++;
                        /* Set X8=0 (no TLBI needed) and continue */
                        HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X8, 0));
                        continue;
                    }
                    return 0;

                } else if (imm == 0) {
                    /* HVC #0: normal exit */
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &out->x0);
                    out->reason = EXIT_HVC0;
                    return 0;

                } else if (imm == 2) {
                    /* HVC #2: bad exception */
                    uint64_t x0, x1, x2, x3, x5;
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1);
                    hv_vcpu_get_reg(vcpu, HV_REG_X2, &x2);
                    hv_vcpu_get_reg(vcpu, HV_REG_X3, &x3);
                    hv_vcpu_get_reg(vcpu, HV_REG_X5, &x5);
                    fprintf(stderr, "  bad exception vec=0x%03llx "
                            "ESR=0x%llx FAR=0x%llx ELR=0x%llx SPSR=0x%llx\n",
                            (unsigned long long)x5,
                            (unsigned long long)x0,
                            (unsigned long long)x1,
                            (unsigned long long)x2,
                            (unsigned long long)x3);
                    out->reason = EXIT_HVC2;
                    return -1;
                }
            }
        }

        /* Any other exit is unexpected */
        fprintf(stderr, "  unexpected exit reason=0x%x ec=0x%llx syndrome=0x%llx\n",
                vexit->reason,
                (unsigned long long)((vexit->exception.syndrome >> 26) & 0x3F),
                (unsigned long long)vexit->exception.syndrome);
        out->reason = EXIT_ERROR;
        return -1;
    }

    fprintf(stderr, "  exceeded max iterations\n");
    out->reason = EXIT_ERROR;
    return -1;
}

/* ── Thread context for concurrent vCPU execution ───────────────── */

typedef struct {
    vm_state_t   *vm;
    uint64_t      ttbr0;
    uint64_t      entry_ipa;
    uint64_t      sp_el0;
    uint64_t      sp_el1;
    uint64_t      shim_ipa;
    int           max_hvc5;     /* How many HVC #5 to handle before stop */
    /* Output */
    hv_vcpu_t     vcpu;
    vcpu_exit_t   exits[8];     /* Recorded HVC #5 exits */
    int           nexit;        /* Number of recorded exits */
    int           result;       /* 0=success, -1=error */
    char          name[16];     /* For debug messages */
} thread_ctx_t;

/* Thread function: create vCPU, configure, run, record exits.
 * HVF requires vCPU operations on the creating thread.
 *
 * Handles HVC #5 exits:
 *   X8=93  → treat as exit (like Linux exit syscall), stop running
 *   other  → marker SVC, record and resume (up to max_hvc5 markers)
 */
static void *vcpu_thread(void *arg) {
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    ctx->nexit = 0;
    ctx->result = -1;

    /* Create vCPU on this thread */
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    hv_return_t ret = hv_vcpu_create(&vcpu, &vexit, NULL);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "  %s: hv_vcpu_create failed: %d\n", ctx->name, (int)ret);
        return NULL;
    }
    ctx->vcpu = vcpu;

    /* Configure vCPU registers */
    if (setup_vcpu(vcpu, ctx->ttbr0, ctx->entry_ipa, ctx->sp_el0,
                   ctx->sp_el1, ctx->shim_ipa) < 0) {
        fprintf(stderr, "  %s: setup_vcpu failed\n", ctx->name);
        hv_vcpu_destroy(vcpu);
        return NULL;
    }

    /* Run until exit. run_vcpu handles HVC #4 internally.
     * We handle HVC #5 here: marker SVCs get resumed, X8=93 stops. */
    int markers_left = ctx->max_hvc5;
    for (;;) {
        vcpu_exit_t ex;
        /* Tell run_vcpu to return on every HVC #5 (max_hvc5=0) so
         * we can inspect X8 and decide whether to resume or stop. */
        int rc = run_vcpu(vcpu, vexit, &ex, 0);

        if (ex.reason == EXIT_HVC5) {
            if (ctx->nexit < 8)
                ctx->exits[ctx->nexit++] = ex;

            if (ex.x8 == 93) {
                /* Exit syscall — guest is done */
                ctx->result = 0;
                break;
            }

            /* Marker SVC — resume the vCPU */
            if (markers_left <= 0) {
                fprintf(stderr, "  %s: too many marker SVCs\n", ctx->name);
                break;
            }
            markers_left--;
            hv_vcpu_set_reg(vcpu, HV_REG_X8, 0);
            continue;
        }

        if (ex.reason == EXIT_HVC0) {
            if (ctx->nexit < 8)
                ctx->exits[ctx->nexit++] = ex;
            ctx->result = 0;
            break;
        }

        /* Error */
        fprintf(stderr, "  %s: run_vcpu error (reason=%d rc=%d)\n",
                ctx->name, ex.reason, rc);
        break;
    }

    hv_vcpu_destroy(vcpu);
    return NULL;
}

/* ── VM lifecycle helpers ───────────────────────────────────────── */

static int vm_create(vm_state_t *vm) {
    vm->host_base = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                         MAP_ANON | MAP_PRIVATE, -1, 0);
    if (vm->host_base == MAP_FAILED) return -1;
    vm->pt_next = PT_POOL_BASE;

    hv_return_t ret = hv_vm_create(NULL);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "  hv_vm_create failed: %d\n", (int)ret);
        munmap(vm->host_base, GUEST_SIZE);
        return -1;
    }

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

/* ════════════════════════════════════════════════════════════════════
 * TEST 1: Basic Dual vCPU Creation
 *
 * Can hv_vcpu_create() be called twice in one VM? Can both vCPUs
 * execute concurrently on separate host threads?
 *
 * Guest code: mov x0, #0; mov x8, #93; svc #0
 * Both vCPUs run the same trivial program, exit via SVC #0 (X8=93).
 * ════════════════════════════════════════════════════════════════════ */

static int test1_basic_dual_vcpu(void) {
    vm_state_t vm;
    if (vm_create(&vm) < 0) return -1;

    uint64_t ttbr0 = build_page_tables(&vm, 0);
    if (!ttbr0) { vm_destroy(&vm); return -1; }

    /* Guest code: mov x0, #0; mov x8, #93; svc #0 */
    uint32_t code[] = { movz(0, 0), movz(8, 93), svc0() };
    emit(vm.host_base, GUEST_CODE + CODE_A_OFF, code, 3);
    emit(vm.host_base, GUEST_CODE + CODE_B_OFF, code, 3);

    /* Launch two threads, each running a vCPU */
    thread_ctx_t ctx_a = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_A_OFF,
        .sp_el0 = SP_EL0_A, .sp_el1 = SP_EL1_A,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 0,
        .name = "vCPU-A"
    };
    thread_ctx_t ctx_b = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_B_OFF,
        .sp_el0 = SP_EL0_B, .sp_el1 = SP_EL1_B,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 0,
        .name = "vCPU-B"
    };

    pthread_t ta, tb;
    pthread_create(&ta, NULL, vcpu_thread, &ctx_a);
    pthread_create(&tb, NULL, vcpu_thread, &ctx_b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    vm_destroy(&vm);

    /* Verify: both vCPUs should have exited with HVC #5 (X8=93) */
    int ok = 1;
    if (ctx_a.result != 0 || ctx_a.nexit < 1) {
        fprintf(stderr, "  vCPU-A failed (result=%d, nexit=%d)\n",
                ctx_a.result, ctx_a.nexit);
        ok = 0;
    } else if (ctx_a.exits[0].x8 != 93) {
        fprintf(stderr, "  vCPU-A X8=%llu (expected 93)\n",
                (unsigned long long)ctx_a.exits[0].x8);
        ok = 0;
    }

    if (ctx_b.result != 0 || ctx_b.nexit < 1) {
        fprintf(stderr, "  vCPU-B failed (result=%d, nexit=%d)\n",
                ctx_b.result, ctx_b.nexit);
        ok = 0;
    } else if (ctx_b.exits[0].x8 != 93) {
        fprintf(stderr, "  vCPU-B X8=%llu (expected 93)\n",
                (unsigned long long)ctx_b.exits[0].x8);
        ok = 0;
    }

    return ok ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════════
 * TEST 2: Shared Memory Writes
 *
 * vCPU-A writes 0xAA to DATA_BASE+0, vCPU-B writes 0xBB to DATA_BASE+8.
 * Host verifies both values after both vCPUs exit.
 * ════════════════════════════════════════════════════════════════════ */

static int test2_shared_memory(void) {
    vm_state_t vm;
    if (vm_create(&vm) < 0) return -1;

    uint64_t ttbr0 = build_page_tables(&vm, 0);
    if (!ttbr0) { vm_destroy(&vm); return -1; }

    /* vCPU-A code: mov x0, #0xAA; mov x1, <DATA_BASE>; str x0, [x1];
     *              mov x0, #0; mov x8, #93; svc #0 */
    {
        uint32_t code[] = {
            movz(0, 0xAA),
            movz(1, (uint16_t)(GUEST_DATA & 0xFFFF)),
            movk_lsl16(1, (uint16_t)((GUEST_DATA >> 16) & 0xFFFF)),
            str_imm(0, 1, 0),   /* str x0, [x1, #0] */
            movz(0, 0),
            movz(8, 93),
            svc0()
        };
        emit(vm.host_base, GUEST_CODE + CODE_A_OFF, code, 7);
    }

    /* vCPU-B code: mov x0, #0xBB; mov x1, <DATA_BASE>; str x0, [x1, #8];
     *              mov x0, #0; mov x8, #93; svc #0 */
    {
        uint32_t code[] = {
            movz(0, 0xBB),
            movz(1, (uint16_t)(GUEST_DATA & 0xFFFF)),
            movk_lsl16(1, (uint16_t)((GUEST_DATA >> 16) & 0xFFFF)),
            str_imm(0, 1, 8),   /* str x0, [x1, #8] */
            movz(0, 0),
            movz(8, 93),
            svc0()
        };
        emit(vm.host_base, GUEST_CODE + CODE_B_OFF, code, 7);
    }

    thread_ctx_t ctx_a = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_A_OFF,
        .sp_el0 = SP_EL0_A, .sp_el1 = SP_EL1_A,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 0,
        .name = "vCPU-A"
    };
    thread_ctx_t ctx_b = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_B_OFF,
        .sp_el0 = SP_EL0_B, .sp_el1 = SP_EL1_B,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 0,
        .name = "vCPU-B"
    };

    pthread_t ta, tb;
    pthread_create(&ta, NULL, vcpu_thread, &ctx_a);
    pthread_create(&tb, NULL, vcpu_thread, &ctx_b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    /* Verify writes in host memory */
    uint64_t val_a = *(uint64_t *)((uint8_t *)vm.host_base + GUEST_DATA);
    uint64_t val_b = *(uint64_t *)((uint8_t *)vm.host_base + GUEST_DATA + 8);

    vm_destroy(&vm);

    int ok = 1;
    if (ctx_a.result != 0) {
        fprintf(stderr, "  vCPU-A failed\n");
        ok = 0;
    }
    if (ctx_b.result != 0) {
        fprintf(stderr, "  vCPU-B failed\n");
        ok = 0;
    }
    if (val_a != 0xAA) {
        fprintf(stderr, "  DATA+0 = 0x%llx (expected 0xAA)\n",
                (unsigned long long)val_a);
        ok = 0;
    }
    if (val_b != 0xBB) {
        fprintf(stderr, "  DATA+8 = 0x%llx (expected 0xBB)\n",
                (unsigned long long)val_b);
        ok = 0;
    }

    return ok ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════════
 * TEST 3: Separate SP_EL1 / Multiple SVCs
 *
 * Each vCPU does two SVCs: a marker SVC (X8=0xAAAA / 0xBBBB) then
 * an exit SVC (X8=93). Exercises the shim's 256-byte SP_EL1 stack
 * frame twice per vCPU, concurrently.
 * ════════════════════════════════════════════════════════════════════ */

static int test3_separate_sp_el1(void) {
    vm_state_t vm;
    if (vm_create(&vm) < 0) return -1;

    uint64_t ttbr0 = build_page_tables(&vm, 0);
    if (!ttbr0) { vm_destroy(&vm); return -1; }

    /* vCPU-A: mov x8, #0xAAAA; svc #0; mov x0, #0; mov x8, #93; svc #0 */
    {
        uint32_t code[] = {
            movz(8, 0xAAAA),
            svc0(),
            movz(0, 0),
            movz(8, 93),
            svc0()
        };
        emit(vm.host_base, GUEST_CODE + CODE_A_OFF, code, 5);
    }

    /* vCPU-B: mov x8, #0xBBBB; svc #0; mov x0, #0; mov x8, #93; svc #0 */
    {
        uint32_t code[] = {
            movz(8, 0xBBBB),
            svc0(),
            movz(0, 0),
            movz(8, 93),
            svc0()
        };
        emit(vm.host_base, GUEST_CODE + CODE_B_OFF, code, 5);
    }

    thread_ctx_t ctx_a = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_A_OFF,
        .sp_el0 = SP_EL0_A, .sp_el1 = SP_EL1_A,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 1,
        .name = "vCPU-A"
    };
    thread_ctx_t ctx_b = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_B_OFF,
        .sp_el0 = SP_EL0_B, .sp_el1 = SP_EL1_B,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 1,
        .name = "vCPU-B"
    };

    pthread_t ta, tb;
    pthread_create(&ta, NULL, vcpu_thread, &ctx_a);
    pthread_create(&tb, NULL, vcpu_thread, &ctx_b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    vm_destroy(&vm);

    int ok = 1;
    if (ctx_a.result != 0 || ctx_a.nexit < 2) {
        fprintf(stderr, "  vCPU-A: result=%d nexit=%d\n", ctx_a.result, ctx_a.nexit);
        ok = 0;
    } else {
        if (ctx_a.exits[0].x8 != 0xAAAA) {
            fprintf(stderr, "  vCPU-A marker X8=0x%llx (expected 0xAAAA)\n",
                    (unsigned long long)ctx_a.exits[0].x8);
            ok = 0;
        }
        if (ctx_a.exits[1].x8 != 93) {
            fprintf(stderr, "  vCPU-A exit X8=%llu (expected 93)\n",
                    (unsigned long long)ctx_a.exits[1].x8);
            ok = 0;
        }
    }

    if (ctx_b.result != 0 || ctx_b.nexit < 2) {
        fprintf(stderr, "  vCPU-B: result=%d nexit=%d\n", ctx_b.result, ctx_b.nexit);
        ok = 0;
    } else {
        if (ctx_b.exits[0].x8 != 0xBBBB) {
            fprintf(stderr, "  vCPU-B marker X8=0x%llx (expected 0xBBBB)\n",
                    (unsigned long long)ctx_b.exits[0].x8);
            ok = 0;
        }
        if (ctx_b.exits[1].x8 != 93) {
            fprintf(stderr, "  vCPU-B exit X8=%llu (expected 93)\n",
                    (unsigned long long)ctx_b.exits[1].x8);
            ok = 0;
        }
    }

    return ok ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════════
 * TEST 4: Register Preservation
 *
 * Each vCPU sets X19=0x1234, X20=0x5678, does SVC (marker), then
 * reports X19/X20 via X0/X1 in a second SVC. Verifies the shim's
 * stp/ldp save/restore on per-vCPU SP_EL1 doesn't corrupt callee-
 * saved registers when two vCPUs run concurrently.
 * ════════════════════════════════════════════════════════════════════ */

static int test4_register_preservation(void) {
    vm_state_t vm;
    if (vm_create(&vm) < 0) return -1;

    uint64_t ttbr0 = build_page_tables(&vm, 0);
    if (!ttbr0) { vm_destroy(&vm); return -1; }

    /* Guest code (same for both vCPUs, different markers via X8):
     *   mov x19, #0x1234
     *   mov x20, #0x5678
     *   mov x8, #<marker>    ; 0xAAAA for A, 0xBBBB for B
     *   svc #0               ; first SVC — shim saves/restores all regs
     *   mov x0, x19           ; report x19 in x0
     *   mov x1, x20           ; report x20 in x1
     *   mov x8, #93
     *   svc #0               ; exit — host reads X0, X1
     */
    {
        uint32_t code_a[] = {
            movz(19, 0x1234),
            movz(20, 0x5678),
            movz(8, 0xAAAA),
            svc0(),
            mov_reg(0, 19),
            mov_reg(1, 20),
            movz(8, 93),
            svc0()
        };
        emit(vm.host_base, GUEST_CODE + CODE_A_OFF, code_a, 8);
    }
    {
        uint32_t code_b[] = {
            movz(19, 0x1234),
            movz(20, 0x5678),
            movz(8, 0xBBBB),
            svc0(),
            mov_reg(0, 19),
            mov_reg(1, 20),
            movz(8, 93),
            svc0()
        };
        emit(vm.host_base, GUEST_CODE + CODE_B_OFF, code_b, 8);
    }

    thread_ctx_t ctx_a = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_A_OFF,
        .sp_el0 = SP_EL0_A, .sp_el1 = SP_EL1_A,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 1,
        .name = "vCPU-A"
    };
    thread_ctx_t ctx_b = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_B_OFF,
        .sp_el0 = SP_EL0_B, .sp_el1 = SP_EL1_B,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 1,
        .name = "vCPU-B"
    };

    pthread_t ta, tb;
    pthread_create(&ta, NULL, vcpu_thread, &ctx_a);
    pthread_create(&tb, NULL, vcpu_thread, &ctx_b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    vm_destroy(&vm);

    int ok = 1;

    /* Check vCPU-A: exit SVC should have X0=0x1234, X1=0x5678 */
    if (ctx_a.result != 0 || ctx_a.nexit < 2) {
        fprintf(stderr, "  vCPU-A: result=%d nexit=%d\n", ctx_a.result, ctx_a.nexit);
        ok = 0;
    } else {
        /* exits[0] = marker SVC, exits[1] = exit SVC with X0/X1 reports */
        if (ctx_a.exits[1].x0 != 0x1234) {
            fprintf(stderr, "  vCPU-A X19→X0=0x%llx (expected 0x1234)\n",
                    (unsigned long long)ctx_a.exits[1].x0);
            ok = 0;
        }
        if (ctx_a.exits[1].x1 != 0x5678) {
            fprintf(stderr, "  vCPU-A X20→X1=0x%llx (expected 0x5678)\n",
                    (unsigned long long)ctx_a.exits[1].x1);
            ok = 0;
        }
    }

    /* Check vCPU-B */
    if (ctx_b.result != 0 || ctx_b.nexit < 2) {
        fprintf(stderr, "  vCPU-B: result=%d nexit=%d\n", ctx_b.result, ctx_b.nexit);
        ok = 0;
    } else {
        if (ctx_b.exits[1].x0 != 0x1234) {
            fprintf(stderr, "  vCPU-B X19→X0=0x%llx (expected 0x1234)\n",
                    (unsigned long long)ctx_b.exits[1].x0);
            ok = 0;
        }
        if (ctx_b.exits[1].x1 != 0x5678) {
            fprintf(stderr, "  vCPU-B X20→X1=0x%llx (expected 0x5678)\n",
                    (unsigned long long)ctx_b.exits[1].x1);
            ok = 0;
        }
    }

    return ok ? 0 : -1;
}

/* ════════════════════════════════════════════════════════════════════
 * TEST 5: TLBI Broadcast
 *
 * Sequential test:
 *   1. Build page tables WITHOUT TLBI_REGION (0x800000) mapped
 *   2. vCPU-A runs, does SVC with marker X8=0xAAAA
 *   3. Host handler: adds L2 entry for 0x800000, writes 0xDEAD there,
 *      sets X8=1 (shim will do TLBI VMALLE1IS + DSB ISH + ISB)
 *   4. vCPU-A resumes, shim does TLBI, ERET to EL0, guest exits
 *   5. vCPU-B starts, reads from 0x800000, reports value via SVC, exits
 *   6. Host verifies vCPU-B read 0xDEAD
 *
 * This test runs vCPUs SEQUENTIALLY (not concurrently) to isolate
 * the TLBI broadcast question: does vCPU-A's TLBI invalidate
 * vCPU-B's TLB when B starts later?
 * ════════════════════════════════════════════════════════════════════ */

static void *vcpu_thread_tlbi_a(void *arg) {
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    ctx->nexit = 0;
    ctx->result = -1;

    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    hv_return_t ret = hv_vcpu_create(&vcpu, &vexit, NULL);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "  %s: hv_vcpu_create failed: %d\n", ctx->name, (int)ret);
        return NULL;
    }
    ctx->vcpu = vcpu;

    if (setup_vcpu(vcpu, ctx->ttbr0, ctx->entry_ipa, ctx->sp_el0,
                   ctx->sp_el1, ctx->shim_ipa) < 0) {
        hv_vcpu_destroy(vcpu);
        return NULL;
    }

    /* Run until first HVC #5 (marker SVC) */
    vcpu_exit_t ex;
    if (run_vcpu(vcpu, vexit, &ex, 0) < 0 || ex.reason != EXIT_HVC5) {
        fprintf(stderr, "  %s: expected HVC #5, got reason=%d\n", ctx->name, ex.reason);
        hv_vcpu_destroy(vcpu);
        return NULL;
    }
    ctx->exits[ctx->nexit++] = ex;

    /* Host: map the TLBI region and write test data */
    add_tlbi_mapping(ctx->vm, ctx->ttbr0);
    *(uint64_t *)((uint8_t *)ctx->vm->host_base + TLBI_REGION) = 0xDEAD;

    /* Set X8=1 so shim does TLBI VMALLE1IS before ERET */
    hv_vcpu_set_reg(vcpu, HV_REG_X8, 1);

    /* Continue running until exit SVC (X8=93) */
    if (run_vcpu(vcpu, vexit, &ex, 0) < 0) {
        fprintf(stderr, "  %s: second run failed\n", ctx->name);
        hv_vcpu_destroy(vcpu);
        return NULL;
    }
    ctx->exits[ctx->nexit++] = ex;
    ctx->result = 0;

    hv_vcpu_destroy(vcpu);
    return NULL;
}

static int test5_tlbi_broadcast(void) {
    vm_state_t vm;
    if (vm_create(&vm) < 0) return -1;

    /* Build page tables WITHOUT TLBI_REGION mapped */
    uint64_t ttbr0 = build_page_tables(&vm, 0);
    if (!ttbr0) { vm_destroy(&vm); return -1; }

    /* vCPU-A code: mov x8, #0xAAAA; svc #0; mov x0, #0; mov x8, #93; svc #0 */
    {
        uint32_t code[] = {
            movz(8, 0xAAAA),
            svc0(),
            movz(0, 0),
            movz(8, 93),
            svc0()
        };
        emit(vm.host_base, GUEST_CODE + CODE_A_OFF, code, 5);
    }

    /* vCPU-B code: load from TLBI_REGION, report via X0, exit
     *   mov x1, <TLBI_REGION>
     *   ldr x0, [x1]
     *   mov x8, #93
     *   svc #0 */
    {
        uint32_t code[] = {
            movz(1, (uint16_t)(TLBI_REGION & 0xFFFF)),
            movk_lsl16(1, (uint16_t)((TLBI_REGION >> 16) & 0xFFFF)),
            ldr_imm(0, 1, 0),  /* ldr x0, [x1] */
            movz(8, 93),
            svc0()
        };
        emit(vm.host_base, GUEST_CODE + CODE_B_OFF, code, 5);
    }

    /* Phase 1: Run vCPU-A (maps the region, does TLBI) */
    thread_ctx_t ctx_a = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_A_OFF,
        .sp_el0 = SP_EL0_A, .sp_el1 = SP_EL1_A,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 0,
        .name = "vCPU-A"
    };

    pthread_t ta;
    pthread_create(&ta, NULL, vcpu_thread_tlbi_a, &ctx_a);
    pthread_join(ta, NULL);

    if (ctx_a.result != 0) {
        fprintf(stderr, "  vCPU-A failed\n");
        vm_destroy(&vm);
        return -1;
    }

    /* Phase 2: Run vCPU-B (reads from the newly-mapped region) */
    thread_ctx_t ctx_b = {
        .vm = &vm, .ttbr0 = ttbr0,
        .entry_ipa = GUEST_CODE + CODE_B_OFF,
        .sp_el0 = SP_EL0_B, .sp_el1 = SP_EL1_B,
        .shim_ipa = SHIM_BASE, .max_hvc5 = 0,
        .name = "vCPU-B"
    };

    pthread_t tb;
    pthread_create(&tb, NULL, vcpu_thread, &ctx_b);
    pthread_join(tb, NULL);

    vm_destroy(&vm);

    int ok = 1;
    if (ctx_b.result != 0 || ctx_b.nexit < 1) {
        fprintf(stderr, "  vCPU-B failed (result=%d nexit=%d)\n",
                ctx_b.result, ctx_b.nexit);
        ok = 0;
    } else {
        /* The exit SVC carries X0 = value read from TLBI_REGION */
        uint64_t read_val = ctx_b.exits[0].x0;
        if (read_val != 0xDEAD) {
            fprintf(stderr, "  vCPU-B read 0x%llx (expected 0xDEAD)\n",
                    (unsigned long long)read_val);
            ok = 0;
        }
    }

    return ok ? 0 : -1;
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(void) {
    printf("Multi-vCPU Validation for HVF (Apple Hypervisor.framework)\n\n");

    int pass = 0, fail = 0;

    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "Basic dual vCPU creation",  test1_basic_dual_vcpu },
        { "Shared memory writes",      test2_shared_memory },
        { "Separate SP_EL1 stacks",    test3_separate_sp_el1 },
        { "Register preservation",     test4_register_preservation },
        { "TLBI broadcast",            test5_tlbi_broadcast },
    };
    int ntests = (int)(sizeof(tests) / sizeof(tests[0]));

    for (int i = 0; i < ntests; i++) {
        printf("TEST %d: %-35s ", i + 1, tests[i].name);
        fflush(stdout);

        int rc = tests[i].fn();
        if (rc == 0) {
            printf(GREEN "PASS" RESET "\n");
            pass++;
        } else {
            printf(RED "FAIL" RESET "\n");
            fail++;

            /* If test 1 fails, stop — multi-vCPU is not viable */
            if (i == 0) {
                printf("\nTest 1 failed — multi-vCPU not viable in HVF. Stopping.\n");
                break;
            }
        }
    }

    printf("\nResults: %d/%d passed\n", pass, ntests);
    return fail > 0 ? 1 : 0;
}
