/* guest.c — Guest memory management for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Identity-mapped guest memory: GVA == GPA == offset into host_base.
 * A single 512MB slab is mapped RWX to Hypervisor.framework. The guest's
 * own page tables (built here) enforce per-region permissions using 2MB
 * block descriptors, which are mandatory for transparent misaligned access.
 *
 * Page table format: AArch64 4KB granule, 3-level (L0 -> L1 -> L2).
 *   L0 entry covers 512GB — one entry pointing to L1
 *   L1 entry covers 1GB  — either block or table pointing to L2
 *   L2 entry covers 2MB  — block descriptors with final permissions
 */
#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------- Page table descriptor bits ---------- */
#define PT_VALID       (1ULL << 0)
#define PT_TABLE       (1ULL << 1)  /* Table descriptor (L0/L1) */
#define PT_BLOCK       (1ULL << 0)  /* Block descriptor (L1/L2): valid bit only */
#define PT_AF          (1ULL << 10) /* Access Flag */
#define PT_SH_ISH      (3ULL << 8)  /* Inner Shareable */
#define PT_NS          (1ULL << 5)  /* Non-Secure */
#define PT_ATTR1       (1ULL << 2)  /* MAIR index 1: Normal WB cacheable */
#define PT_UXN         (1ULL << 54) /* Unprivileged Execute Never */
#define PT_PXN         (1ULL << 53) /* Privileged Execute Never */
#define PT_AP_RW_EL0   (1ULL << 6)  /* AP[2:1]=01: RW at EL1, RW at EL0 */
#define PT_AP_RO       (3ULL << 6)  /* AP[2:1]=11: RO at EL1, RO at EL0 */

#define PAGE_SIZE      4096ULL
#define BLOCK_2MB      (2ULL * 1024 * 1024)
#define BLOCK_1GB      (1ULL * 1024 * 1024 * 1024)

/* Round up/down to 2MB boundary */
#define ALIGN_2MB_DOWN(x) ((x) & ~(BLOCK_2MB - 1))
#define ALIGN_2MB_UP(x)   (((x) + BLOCK_2MB - 1) & ~(BLOCK_2MB - 1))

/* ---------- Page table pool allocator ---------- */

/* Allocate a zeroed 4KB page from the page table pool.
 * Returns GPA of the page, or 0 on pool exhaustion. */
static uint64_t pt_alloc_page(guest_t *g) {
    if (g->pt_pool_next + PAGE_SIZE > PT_POOL_END) {
        fprintf(stderr, "guest: page table pool exhausted\n");
        return 0;
    }
    uint64_t gpa = g->pt_pool_next;
    g->pt_pool_next += PAGE_SIZE;
    /* Zero the page in host memory */
    memset((uint8_t *)g->host_base + gpa, 0, PAGE_SIZE);
    return gpa;
}

/* Get host pointer to a page table entry array at a given GPA */
static uint64_t *pt_at(const guest_t *g, uint64_t gpa) {
    return (uint64_t *)((uint8_t *)g->host_base + gpa);
}

/* ---------- Public API ---------- */

int guest_init(guest_t *g, uint64_t size) {
    memset(g, 0, sizeof(*g));
    g->guest_size = size;
    g->ipa_base = GUEST_IPA_BASE;
    g->pt_pool_next = PT_POOL_BASE;
    g->brk_base = BRK_BASE_DEFAULT;
    g->brk_current = BRK_BASE_DEFAULT;
    g->mmap_next = MMAP_BASE;

    /* Allocate page-aligned host memory */
    if (posix_memalign(&g->host_base, getpagesize(), size) != 0) {
        perror("guest: posix_memalign");
        return -1;
    }
    memset(g->host_base, 0, size);

    /* Create Hypervisor VM and map the entire slab at GUEST_IPA_BASE */
    hv_return_t ret = hv_vm_create(NULL);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "guest: hv_vm_create failed: %d\n", (int)ret);
        free(g->host_base);
        return -1;
    }

    ret = hv_vm_map(g->host_base, GUEST_IPA_BASE, size,
                    HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "guest: hv_vm_map failed: %d\n", (int)ret);
        hv_vm_destroy();
        free(g->host_base);
        return -1;
    }

    return 0;
}

void guest_destroy(guest_t *g) {
    if (g->vcpu) {
        hv_vcpu_destroy(g->vcpu);
        g->vcpu = 0;
    }
    hv_vm_destroy();
    if (g->host_base) {
        free(g->host_base);
        g->host_base = NULL;
    }
}

/* Convert a guest virtual address (which may be IPA-based) to a host offset */
static uint64_t gva_to_offset(const guest_t *g, uint64_t gva) {
    if (gva >= g->ipa_base && gva < g->ipa_base + g->guest_size)
        return gva - g->ipa_base;
    /* Legacy: allow raw offsets too (for internal use) */
    if (gva < g->guest_size)
        return gva;
    return UINT64_MAX; /* Invalid */
}

void *guest_ptr(const guest_t *g, uint64_t gva) {
    uint64_t off = gva_to_offset(g, gva);
    if (off == UINT64_MAX || off >= g->guest_size)
        return NULL;
    return (uint8_t *)g->host_base + off;
}

int guest_read(const guest_t *g, uint64_t gva, void *dst, size_t len) {
    uint64_t off = gva_to_offset(g, gva);
    if (off == UINT64_MAX || off + len > g->guest_size || off + len < off)
        return -1;
    memcpy(dst, (const uint8_t *)g->host_base + off, len);
    return 0;
}

int guest_write(guest_t *g, uint64_t gva, const void *src, size_t len) {
    uint64_t off = gva_to_offset(g, gva);
    if (off == UINT64_MAX || off + len > g->guest_size || off + len < off)
        return -1;
    memcpy((uint8_t *)g->host_base + off, src, len);
    return 0;
}

int guest_read_str(const guest_t *g, uint64_t gva, char *dst, size_t max) {
    uint64_t off = gva_to_offset(g, gva);
    if (off == UINT64_MAX || off >= g->guest_size)
        return -1;
    const char *src = (const char *)g->host_base + off;
    size_t avail = g->guest_size - off;
    if (avail > max - 1)
        avail = max - 1;

    size_t i;
    for (i = 0; i < avail; i++) {
        dst[i] = src[i];
        if (src[i] == '\0')
            return (int)i;
    }
    /* Unterminated within bounds */
    dst[i] = '\0';
    return -1;
}

/* ---------- Page table builder ---------- */

/* Build block descriptor for a 2MB block at the given GPA with perms. */
static uint64_t make_block_desc(uint64_t gpa, int perms) {
    uint64_t desc = (gpa & 0xFFFFFFFFE00000ULL) /* PA bits */
                  | PT_AF
                  | PT_SH_ISH
                  | PT_NS
                  | PT_ATTR1   /* Normal WB cacheable */
                  | PT_BLOCK;  /* Valid block */

    /* Execute permissions: XN bits disable execution */
    if (!(perms & MEM_PERM_X)) {
        desc |= PT_UXN | PT_PXN;
    }

    /* Write permissions via AP bits:
     * AP[2:1]=01 → RW for EL1 and EL0
     * AP[2:1]=11 → RO for EL1 and EL0  */
    if (perms & MEM_PERM_W) {
        desc |= PT_AP_RW_EL0;
    } else {
        desc |= PT_AP_RO;
    }

    return desc;
}

uint64_t guest_build_page_tables(guest_t *g, const mem_region_t *regions, int n) {
    uint64_t base = g->ipa_base;

    /* Allocate L0 table */
    uint64_t l0_gpa = pt_alloc_page(g);
    if (!l0_gpa) return 0;

    uint64_t *l0 = pt_at(g, l0_gpa);

    /* L1 table — determine which L0 entry we need based on ipa_base.
     * All our addresses are ipa_base + offset, so they fall in one L0 slot. */
    uint64_t l1_gpa = pt_alloc_page(g);
    if (!l1_gpa) return 0;

    unsigned l0_idx = (unsigned)(base / (512ULL * BLOCK_1GB));
    l0[l0_idx] = (base + l1_gpa) | PT_VALID | PT_TABLE;
    uint64_t *l1 = pt_at(g, l1_gpa);

    /* For each region, determine which 2MB blocks need mapping.
     * Regions use offsets (0-based). We add ipa_base for IPA addresses
     * in page table entries. The L1/L2 indices are relative to ipa_base. */
    for (int r = 0; r < n; r++) {
        uint64_t start = ALIGN_2MB_DOWN(regions[r].gpa_start);
        uint64_t end   = ALIGN_2MB_UP(regions[r].gpa_end);
        int perms = regions[r].perms;

        for (uint64_t addr = start; addr < end; addr += BLOCK_2MB) {
            /* Compute IPA for this block */
            uint64_t ipa = base + addr;

            /* L1 index within the 512GB L0 entry */
            unsigned l1_idx = (unsigned)((ipa % (512ULL * BLOCK_1GB)) / BLOCK_1GB);
            if (l1_idx >= 512) {
                fprintf(stderr, "guest: IPA 0x%llx out of L1 range\n",
                        (unsigned long long)ipa);
                continue;
            }

            /* Ensure L1 entry points to an L2 table */
            if (!(l1[l1_idx] & PT_VALID)) {
                uint64_t l2_gpa = pt_alloc_page(g);
                if (!l2_gpa) return 0;
                l1[l1_idx] = (base + l2_gpa) | PT_VALID | PT_TABLE;
            }

            /* L2 table for this 1GB region (stored in host at gpa offset) */
            uint64_t l2_ipa = l1[l1_idx] & 0xFFFFFFFFF000ULL;
            uint64_t l2_gpa_off = l2_ipa - base;
            uint64_t *l2 = pt_at(g, l2_gpa_off);

            /* L2 index: which 2MB block within the 1GB region */
            unsigned l2_idx = (unsigned)((ipa % BLOCK_1GB) / BLOCK_2MB);

            /* If block already mapped, merge permissions (most permissive) */
            if (l2[l2_idx] & PT_BLOCK) {
                int old_perms = 0;
                if (!(l2[l2_idx] & PT_UXN)) old_perms |= MEM_PERM_X;
                if ((l2[l2_idx] & (3ULL << 6)) == PT_AP_RW_EL0)
                    old_perms |= MEM_PERM_W;
                old_perms |= MEM_PERM_R;
                perms |= old_perms;
            }

            /* Block descriptor uses IPA as the output address */
            l2[l2_idx] = make_block_desc(ipa, perms);
        }
    }

    /* Return the IPA of the L0 table (for TTBR0) */
    return base + l0_gpa;
}
