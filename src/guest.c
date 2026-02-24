/* guest.c — Guest memory management for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Identity-mapped guest memory: GVA == GPA == offset into host_base.
 * The guest address space size is determined at runtime by querying the
 * max IPA size via hv_vm_config_get_max_ipa_size(): 40-bit (1TB) on
 * macOS 15+, falling back to 36-bit (64GB). The space is reserved via
 * mmap(MAP_ANON); macOS demand-pages physical memory on first touch, so
 * only used pages consume RAM. The slab is mapped RWX to
 * Hypervisor.framework. The guest's own page tables
 * (built here) enforce per-region permissions using 2MB block descriptors,
 * which are mandatory for transparent misaligned access. Page tables can be
 * extended at runtime via guest_extend_page_tables().
 *
 * PROT_NONE mappings (used by GHC RTS for heap reservation) do NOT get
 * page table entries — the translation fault is the correct behavior.
 * When mprotect changes an accessible region to PROT_NONE,
 * guest_invalidate_ptes() removes existing page table entries.
 * Page tables are created on demand when mprotect changes PROT_NONE
 * to an accessible permission.
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
#include <pthread.h>
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

/* Protects the page table pool bump allocator. Multiple threads may
 * trigger page table extension concurrently (via mmap/brk/mprotect). */
static pthread_mutex_t pt_lock = PTHREAD_MUTEX_INITIALIZER; /* Lock order: 2 */

/* Track whether the 80% warning has been emitted (avoid log spam) */
static int pt_pool_warned = 0;

/* Allocate a zeroed 4KB page from the page table pool.
 * Returns GPA of the page, or 0 on pool exhaustion.
 * Caller must hold pt_lock or the higher-level mmap_lock. */
static uint64_t pt_alloc_page(guest_t *g) {
    pthread_mutex_lock(&pt_lock);
    if (g->pt_pool_next + PAGE_SIZE > PT_POOL_END) {
        fprintf(stderr, "guest: page table pool exhausted "
                "(used %llu / %llu bytes)\n",
                (unsigned long long)(g->pt_pool_next - PT_POOL_BASE),
                (unsigned long long)(PT_POOL_END - PT_POOL_BASE));
        pthread_mutex_unlock(&pt_lock);
        return 0;
    }
    uint64_t gpa = g->pt_pool_next;
    g->pt_pool_next += PAGE_SIZE;

    /* Warn at 80% pool usage so users can anticipate exhaustion */
    uint64_t used = gpa + PAGE_SIZE - PT_POOL_BASE;
    uint64_t total = PT_POOL_END - PT_POOL_BASE;
    if (!pt_pool_warned && used > (total * 4 / 5)) {
        fprintf(stderr, "guest: warning: page table pool at %llu%% "
                "(%llu / %llu bytes)\n",
                (unsigned long long)(used * 100 / total),
                (unsigned long long)used,
                (unsigned long long)total);
        pt_pool_warned = 1;
    }

    /* Zero the page while still holding the lock so no other thread
     * can observe a partially-zeroed page table page. */
    memset((uint8_t *)g->host_base + gpa, 0, PAGE_SIZE);
    pthread_mutex_unlock(&pt_lock);
    return gpa;
}

/* Get host pointer to a page table entry array at a given GPA */
static uint64_t *pt_at(const guest_t *g, uint64_t gpa) {
    return (uint64_t *)((uint8_t *)g->host_base + gpa);
}

/* ---------- Public API ---------- */

int guest_init(guest_t *g, uint64_t size) {
    memset(g, 0, sizeof(*g));
    g->ipa_base = GUEST_IPA_BASE;
    g->pt_pool_next = PT_POOL_BASE;
    g->brk_base = BRK_BASE_DEFAULT;
    g->brk_current = BRK_BASE_DEFAULT;
    g->mmap_next = MMAP_BASE;
    g->mmap_rx_next = MMAP_RX_BASE;

    /* Query the maximum IPA size supported by the hardware/kernel.
     * macOS 15+ on Apple Silicon reports 40 bits (1TB). Older versions
     * or fallback yields 36 bits (64GB). This costs one extra level of
     * stage-2 page tables in HVF but is kernel-internal — our guest
     * page tables are unaffected. */
    uint32_t max_ipa = 0;
    hv_return_t ipa_ret = hv_vm_config_get_max_ipa_size(&max_ipa);
    if (ipa_ret != HV_SUCCESS || max_ipa < 36) max_ipa = 36;
    else if (max_ipa >= 40) max_ipa = 40;

    uint64_t ipa_capacity = 1ULL << max_ipa;

    /* If caller provided an explicit size, clamp to IPA capacity.
     * size==0 means "auto-detect from IPA". */
    if (size == 0 || size > ipa_capacity)
        size = ipa_capacity;
    g->guest_size = size;

    /* Compute dynamic layout limits from guest_size.
     * interp_base: last 4GB (dynamic linker load address)
     * mmap_limit:  last 8GB reserved (max mmap RW address)
     * For 64GB:  interp=60GB, mmap_limit=56GB (matches old constants)
     * For 1TB:   interp=1020GB, mmap_limit=1016GB */
    g->interp_base = g->guest_size - 0x100000000ULL;
    g->mmap_limit  = g->guest_size - 0x200000000ULL;

    /* Reserve address space via mmap(MAP_ANON). macOS demand-pages this:
     * physical pages are allocated only on first touch, so reserving up
     * to 1TB costs nothing until pages are actually used. Do NOT memset
     * — that would touch all pages and defeat demand paging. */
    g->host_base = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
    if (g->host_base == MAP_FAILED) {
        perror("guest: mmap");
        g->host_base = NULL;
        return -1;
    }

    /* Create Hypervisor VM with the detected IPA size and map the
     * entire slab at GUEST_IPA_BASE */
    hv_vm_config_t config = hv_vm_config_create();
    hv_vm_config_set_ipa_size(config, max_ipa);
    hv_return_t ret = hv_vm_create(config);
    os_release(config);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "guest: hv_vm_create failed: %d (ipa_bits=%u)\n",
                (int)ret, max_ipa);
        munmap(g->host_base, size);
        g->host_base = NULL;
        return -1;
    }

    ret = hv_vm_map(g->host_base, GUEST_IPA_BASE, size,
                    HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "guest: hv_vm_map failed: %d\n", (int)ret);
        hv_vm_destroy();
        munmap(g->host_base, size);
        g->host_base = NULL;
        return -1;
    }

    fprintf(stderr, "guest: IPA size: %u bits (%lluGB)\n",
            max_ipa, (unsigned long long)(size / (1024ULL * 1024 * 1024)));

    return 0;
}

void guest_destroy(guest_t *g) {
    if (g->vcpu) {
        hv_vcpu_destroy(g->vcpu);
        g->vcpu = 0;
    }
    hv_vm_destroy();
    if (g->host_base) {
        munmap(g->host_base, g->guest_size);
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

    /* Scan for NUL terminator in the host-mapped buffer directly.
     * Since guest memory is identity-mapped, we can use memchr on
     * the host buffer instead of copying byte-by-byte. */
    const void *nul = memchr(src, '\0', avail);
    if (nul) {
        size_t len = (const char *)nul - src;
        memcpy(dst, src, len + 1); /* Include the NUL terminator */
        return (int)len;
    }
    /* Unterminated within bounds */
    memcpy(dst, src, avail);
    dst[avail] = '\0';
    return -1;
}

/* ---------- guest_reset ---------- */

void guest_reset(guest_t *g) {
    /* Zero only actually-used memory regions. With a potentially 1TB
     * address space, memset of the entire range would fault in all
     * demand-paged memory for no benefit. PROT_NONE regions (e.g., GHC's
     * heap reservation) were never written to, so they're already in the
     * MAP_ANON zero-fill-on-demand state. */

    /* Zero tracked regions (ELF segments, heap, stack, mmap allocations).
     * Skip PROT_NONE regions — they were never touched. */
    for (int i = 0; i < g->nregions; i++) {
        guest_region_t *r = &g->regions[i];
        if (r->prot != 0 /* PROT_NONE */ && r->end > r->start) {
            memset((uint8_t *)g->host_base + r->start, 0,
                   r->end - r->start);
        }
    }

    /* Zero page table pool (not tracked in region array) */
    if (g->pt_pool_next > PT_POOL_BASE)
        memset((uint8_t *)g->host_base + PT_POOL_BASE, 0,
               g->pt_pool_next - PT_POOL_BASE);

    /* Zero shim code + data (not tracked in region array by guest_reset
     * callers — shim regions are added AFTER reset by the exec path) */
    memset((uint8_t *)g->host_base + SHIM_BASE, 0,
           SHIM_DATA_BASE + BLOCK_2MB - SHIM_BASE);

    /* Reset allocation state */
    g->pt_pool_next = PT_POOL_BASE;
    g->brk_base = BRK_BASE_DEFAULT;
    g->brk_current = BRK_BASE_DEFAULT;
    g->mmap_next = MMAP_BASE;
    g->mmap_end = MMAP_INITIAL_END;
    g->mmap_rx_next = MMAP_RX_BASE;
    g->mmap_rx_end = MMAP_RX_INITIAL_END;
    g->ttbr0 = 0;
    g->need_tlbi = 0;

    /* Clear semantic region tracking (will be re-populated after exec) */
    guest_region_clear(g);
}

/* ---------- Used region enumeration ---------- */

int guest_get_used_regions(const guest_t *g, unsigned int shim_size,
                           used_region_t *out, int max) {
    int n = 0;

    /* Page table pool */
    if (n < max && g->pt_pool_next > PT_POOL_BASE) {
        out[n].offset = PT_POOL_BASE;
        out[n].size = g->pt_pool_next - PT_POOL_BASE;
        n++;
    }

    /* Shim code */
    if (n < max && shim_size > 0) {
        out[n].offset = SHIM_BASE;
        out[n].size = shim_size;
        n++;
    }

    /* Shim data/stack (full 2MB block) */
    if (n < max) {
        out[n].offset = SHIM_DATA_BASE;
        out[n].size = BLOCK_2MB;
        n++;
    }

    /* ELF + brk region: from ELF_DEFAULT_BASE to brk_current.
     * We don't track the exact ELF load range, but static musl binaries
     * always load at or above ELF_DEFAULT_BASE (0x400000). */
    if (n < max && g->brk_current > ELF_DEFAULT_BASE) {
        out[n].offset = ELF_DEFAULT_BASE;
        out[n].size = g->brk_current - ELF_DEFAULT_BASE;
        n++;
    }

    /* Stack (2MB block) */
    if (n < max) {
        out[n].offset = STACK_BASE;
        out[n].size = STACK_TOP - STACK_BASE;
        n++;
    }

    /* mmap region (up to high-water mark). With the gap-finding allocator,
     * mmap_next is a high-water mark — freed regions within this range may
     * contain PROT_NONE pages (zero-fill, no cost to copy). This is
     * conservative but correct for fork state transfer. */
    if (n < max && g->mmap_next > MMAP_BASE) {
        out[n].offset = MMAP_BASE;
        out[n].size = g->mmap_next - MMAP_BASE;
        n++;
    }

    return n;
}

/* ---------- Semantic region tracking ---------- */

/* Check whether two adjacent regions can be merged. They must be
 * contiguous in address space, have identical protection/flags/name,
 * and have contiguous file offsets (so the merged region still
 * represents a valid mapping). */
static int regions_mergeable(const guest_region_t *a,
                              const guest_region_t *b) {
    return a->end == b->start
        && a->prot == b->prot
        && a->flags == b->flags
        && a->offset + (a->end - a->start) == b->offset
        && strcmp(a->name, b->name) == 0;
}

/* Try to merge region at index i with its right neighbor (i+1).
 * Returns 1 if merged (nregions decremented), 0 otherwise. */
static int try_merge_right(guest_t *g, int i) {
    if (i + 1 >= g->nregions) return 0;
    if (!regions_mergeable(&g->regions[i], &g->regions[i + 1])) return 0;

    g->regions[i].end = g->regions[i + 1].end;
    memmove(&g->regions[i + 1], &g->regions[i + 2],
            (g->nregions - i - 2) * sizeof(guest_region_t));
    g->nregions--;
    return 1;
}

/* Try to merge region at index i with its left neighbor (i-1).
 * Returns 1 if merged (nregions decremented, region now at i-1), 0 otherwise. */
static int try_merge_left(guest_t *g, int i) {
    if (i <= 0) return 0;
    if (!regions_mergeable(&g->regions[i - 1], &g->regions[i])) return 0;

    g->regions[i - 1].end = g->regions[i].end;
    memmove(&g->regions[i], &g->regions[i + 1],
            (g->nregions - i - 1) * sizeof(guest_region_t));
    g->nregions--;
    return 1;
}

int guest_region_add(guest_t *g, uint64_t start, uint64_t end,
                     int prot, int flags, uint64_t offset,
                     const char *name) {
    if (g->nregions >= GUEST_MAX_REGIONS) return -1;

    /* Find insertion point (keep sorted by start address) */
    int i = g->nregions;
    while (i > 0 && g->regions[i - 1].start > start) {
        g->regions[i] = g->regions[i - 1];
        i--;
    }

    guest_region_t *r = &g->regions[i];
    r->start  = start;
    r->end    = end;
    r->prot   = prot;
    r->flags  = flags;
    r->offset = offset;
    if (name) {
        strncpy(r->name, name, sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
    } else {
        r->name[0] = '\0';
    }
    g->nregions++;

    /* Try to merge with adjacent regions to reduce table pressure.
     * Merge right first, then left (order matters: right merge doesn't
     * change the index of the left neighbor). */
    try_merge_right(g, i);
    try_merge_left(g, i);

    return 0;
}

void guest_region_remove(guest_t *g, uint64_t start, uint64_t end) {
    int i = 0;
    while (i < g->nregions) {
        guest_region_t *r = &g->regions[i];

        /* No overlap: region is entirely before the removal range */
        if (r->end <= start) { i++; continue; }

        /* No overlap: region is entirely after the removal range */
        if (r->start >= end) break; /* sorted, so done */

        /* Full containment: remove the entire region */
        if (r->start >= start && r->end <= end) {
            memmove(&g->regions[i], &g->regions[i + 1],
                    (g->nregions - i - 1) * sizeof(guest_region_t));
            g->nregions--;
            continue; /* don't increment i */
        }

        /* Partial overlap: removal range cuts the beginning */
        if (r->start >= start && r->start < end && r->end > end) {
            uint64_t trimmed = end - r->start;
            r->offset += trimmed;
            r->start = end;
            i++;
            continue;
        }

        /* Partial overlap: removal range cuts the end */
        if (r->start < start && r->end > start && r->end <= end) {
            r->end = start;
            i++;
            continue;
        }

        /* Split: removal range is entirely inside the region */
        if (r->start < start && r->end > end) {
            /* Need to split into two regions: [r->start, start) and [end, r->end) */
            if (g->nregions >= GUEST_MAX_REGIONS) {
                /* Can't split — just trim to [r->start, start) and lose the tail */
                r->end = start;
                i++;
                continue;
            }
            /* Make room for the new region after i */
            memmove(&g->regions[i + 2], &g->regions[i + 1],
                    (g->nregions - i - 1) * sizeof(guest_region_t));

            /* Right half: [end, old_end) */
            guest_region_t *right = &g->regions[i + 1];
            *right = *r;  /* Copy attributes */
            right->offset += (end - r->start);
            right->start = end;

            /* Left half: [r->start, start) — just trim end */
            r->end = start;

            g->nregions++;
            i += 2; /* skip both halves */
            continue;
        }

        i++;
    }
}

const guest_region_t *guest_region_find(const guest_t *g, uint64_t addr) {
    /* Binary search in sorted array */
    int lo = 0, hi = g->nregions - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (addr < g->regions[mid].start) {
            hi = mid - 1;
        } else if (addr >= g->regions[mid].end) {
            lo = mid + 1;
        } else {
            return &g->regions[mid];
        }
    }
    return NULL;
}

void guest_region_set_prot(guest_t *g, uint64_t start, uint64_t end, int prot) {
    /* Walk regions overlapping [start, end), split at boundaries, update prot.
     * Track the range of indices that were modified so we can merge afterward. */
    int first_modified = -1, last_modified = -1;

    for (int i = 0; i < g->nregions; i++) {
        guest_region_t *r = &g->regions[i];
        if (r->end <= start) continue;
        if (r->start >= end) break;

        /* If region extends before start, split at start */
        if (r->start < start) {
            if (g->nregions >= GUEST_MAX_REGIONS) continue;
            memmove(&g->regions[i + 1], &g->regions[i],
                    (g->nregions - i) * sizeof(guest_region_t));
            g->nregions++;
            /* Left half keeps original prot */
            g->regions[i].end = start;
            /* Right half will be processed next iteration */
            g->regions[i + 1].offset += (start - g->regions[i + 1].start);
            g->regions[i + 1].start = start;
            i++; /* advance to the right half */
            r = &g->regions[i];
        }

        /* If region extends past end, split at end */
        if (r->end > end) {
            if (g->nregions >= GUEST_MAX_REGIONS) {
                r->prot = prot;
                if (first_modified < 0) first_modified = i;
                last_modified = i;
                continue;
            }
            memmove(&g->regions[i + 1], &g->regions[i],
                    (g->nregions - i) * sizeof(guest_region_t));
            g->nregions++;
            /* Left half: [r->start, end) with new prot */
            g->regions[i].end = end;
            g->regions[i].prot = prot;
            /* Right half: [end, old_end) keeps original prot */
            g->regions[i + 1].offset += (end - g->regions[i + 1].start);
            g->regions[i + 1].start = end;
            if (first_modified < 0) first_modified = i;
            last_modified = i;
            break; /* done, right half is past our range */
        }

        /* Region fully within [start, end): just update prot */
        r->prot = prot;
        if (first_modified < 0) first_modified = i;
        last_modified = i;
    }

    /* After updating prot, try to merge modified regions with neighbors.
     * Work right-to-left so index shifts don't invalidate earlier indices. */
    if (first_modified >= 0) {
        /* Merge last modified with its right neighbor */
        try_merge_right(g, last_modified);
        /* Merge adjacent modified regions (right to left) */
        for (int i = last_modified; i > first_modified; i--)
            try_merge_left(g, i);
        /* Merge first modified with its left neighbor */
        try_merge_left(g, first_modified);
    }
}

void guest_region_clear(guest_t *g) {
    g->nregions = 0;
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

    /* For each region, determine which 2MB blocks need mapping.
     * Regions use offsets (0-based). We add ipa_base for IPA addresses
     * in page table entries. L0/L1/L2 indices are derived from the IPA.
     * For >512GB address spaces, addresses span multiple L0 entries
     * (each covering 512GB), so L1 tables are allocated on demand. */
    for (int r = 0; r < n; r++) {
        uint64_t start = ALIGN_2MB_DOWN(regions[r].gpa_start);
        uint64_t end   = ALIGN_2MB_UP(regions[r].gpa_end);
        int perms = regions[r].perms;

        for (uint64_t addr = start; addr < end; addr += BLOCK_2MB) {
            /* Compute IPA for this block */
            uint64_t ipa = base + addr;

            /* L0 index: which 512GB slot this IPA falls in */
            unsigned l0_idx = (unsigned)(ipa / (512ULL * BLOCK_1GB));
            if (l0_idx >= 512) {
                fprintf(stderr, "guest: IPA 0x%llx out of L0 range\n",
                        (unsigned long long)ipa);
                continue;
            }

            /* Allocate L1 table on first access to each L0 slot */
            if (!(l0[l0_idx] & PT_VALID)) {
                uint64_t l1_gpa = pt_alloc_page(g);
                if (!l1_gpa) return 0;
                l0[l0_idx] = (base + l1_gpa) | PT_VALID | PT_TABLE;
            }
            uint64_t l1_ipa = l0[l0_idx] & 0xFFFFFFFFF000ULL;
            uint64_t *l1 = pt_at(g, l1_ipa - base);

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

    /* Store TTBR0 for later use by guest_extend_page_tables */
    uint64_t ttbr0 = base + l0_gpa;
    g->ttbr0 = ttbr0;
    return ttbr0;
}

/* Extend page tables to cover [start, end) with 2MB block descriptors.
 * Walks the existing L0→L1 structure (from g->ttbr0) and allocates new
 * L2 tables as needed. This is safe to call while the vCPU is paused
 * (during HVC #5 handling). Sets g->need_tlbi so the shim flushes the
 * TLB before returning to EL0. */
int guest_extend_page_tables(guest_t *g, uint64_t start, uint64_t end, int perms) {
    uint64_t base = g->ipa_base;

    /* Navigate to L0 table */
    uint64_t l0_gpa_off = g->ttbr0 - base;
    uint64_t *l0 = pt_at(g, l0_gpa_off);

    /* Walk 2MB blocks in [start, end) */
    uint64_t addr_start = ALIGN_2MB_DOWN(start);
    uint64_t addr_end = ALIGN_2MB_UP(end);

    for (uint64_t addr = addr_start; addr < addr_end; addr += BLOCK_2MB) {
        uint64_t ipa = base + addr;

        /* L0 index: which 512GB slot (>512GB addresses need L0[1]+) */
        unsigned l0_idx = (unsigned)(ipa / (512ULL * BLOCK_1GB));

        /* Allocate L1 table on first access to each L0 slot */
        if (!(l0[l0_idx] & PT_VALID)) {
            uint64_t l1_gpa = pt_alloc_page(g);
            if (!l1_gpa) return -1;
            l0[l0_idx] = (base + l1_gpa) | PT_VALID | PT_TABLE;
        }

        uint64_t l1_ipa = l0[l0_idx] & 0xFFFFFFFFF000ULL;
        uint64_t *l1 = pt_at(g, l1_ipa - base);

        unsigned l1_idx = (unsigned)((ipa % (512ULL * BLOCK_1GB)) / BLOCK_1GB);
        if (l1_idx >= 512) {
            fprintf(stderr, "guest: IPA 0x%llx out of L1 range in extend\n",
                    (unsigned long long)ipa);
            return -1;
        }

        /* Ensure L1 entry points to an L2 table */
        if (!(l1[l1_idx] & PT_VALID)) {
            uint64_t l2_gpa = pt_alloc_page(g);
            if (!l2_gpa) return -1;
            l1[l1_idx] = (base + l2_gpa) | PT_VALID | PT_TABLE;
        }

        /* Navigate to L2 table */
        uint64_t l2_ipa = l1[l1_idx] & 0xFFFFFFFFF000ULL;
        uint64_t *l2 = pt_at(g, l2_ipa - base);

        unsigned l2_idx = (unsigned)((ipa % BLOCK_1GB) / BLOCK_2MB);

        /* Only map if not already mapped */
        if (!(l2[l2_idx] & PT_BLOCK)) {
            l2[l2_idx] = make_block_desc(ipa, perms);
        }
    }

    g->need_tlbi = 1;
    return 0;
}

/* ---------- L3 page table splitting ---------- */

/* L3 page descriptor: bits[1:0]=11 = valid page at level 3.
 * This is distinct from L2 block descriptors (bits[1:0]=01). */
#define PT_L3_PAGE (3ULL)

/* Build a 4KB L3 page descriptor with the given permissions.
 * Layout matches block descriptors (AF, SH, NS, MAIR, AP, XN)
 * except bits[1:0]=11 instead of 01. */
static uint64_t make_page_desc(uint64_t pa, int perms) {
    uint64_t desc = (pa & 0xFFFFFFFFF000ULL) /* PA bits [47:12] */
                  | PT_AF
                  | PT_SH_ISH
                  | PT_NS
                  | PT_ATTR1
                  | PT_L3_PAGE;

    if (!(perms & MEM_PERM_X))
        desc |= PT_UXN | PT_PXN;

    if (perms & MEM_PERM_W)
        desc |= PT_AP_RW_EL0;
    else
        desc |= PT_AP_RO;

    return desc;
}

/* Extract MEM_PERM_* flags from a page table descriptor (block or page). */
static int desc_to_perms(uint64_t desc) {
    int perms = MEM_PERM_R;
    if (!(desc & PT_UXN))
        perms |= MEM_PERM_X;
    if ((desc & (3ULL << 6)) == PT_AP_RW_EL0)
        perms |= MEM_PERM_W;
    return perms;
}

/* Navigate L0→L1→L2 to find the L2 entry for a given GPA offset.
 * Returns a pointer to the L2 entry, or NULL if not mapped. */
static uint64_t *find_l2_entry(guest_t *g, uint64_t gpa_offset) {
    uint64_t base = g->ipa_base;
    uint64_t ipa = base + gpa_offset;

    uint64_t l0_gpa_off = g->ttbr0 - base;
    uint64_t *l0 = pt_at(g, l0_gpa_off);

    /* L0 index from actual IPA (not base), correct for >512GB */
    unsigned l0_idx = (unsigned)(ipa / (512ULL * BLOCK_1GB));
    if (!(l0[l0_idx] & PT_VALID)) return NULL;

    uint64_t l1_ipa = l0[l0_idx] & 0xFFFFFFFFF000ULL;
    uint64_t *l1 = pt_at(g, l1_ipa - base);

    unsigned l1_idx = (unsigned)((ipa % (512ULL * BLOCK_1GB)) / BLOCK_1GB);
    if (l1_idx >= 512 || !(l1[l1_idx] & PT_VALID)) return NULL;

    uint64_t l2_ipa = l1[l1_idx] & 0xFFFFFFFFF000ULL;
    uint64_t *l2 = pt_at(g, l2_ipa - base);

    unsigned l2_idx = (unsigned)((ipa % BLOCK_1GB) / BLOCK_2MB);
    return &l2[l2_idx];
}

int guest_split_block(guest_t *g, uint64_t block_gpa) {
    uint64_t base = g->ipa_base;
    uint64_t block_start = ALIGN_2MB_DOWN(block_gpa);

    uint64_t *l2_entry = find_l2_entry(g, block_start);
    if (!l2_entry) return -1;

    /* Already a table descriptor (previously split) — nothing to do */
    if ((*l2_entry & 3) == 3) return 0;

    /* Must be a valid block descriptor: bit[0]=1, bit[1]=0 */
    if (!(*l2_entry & PT_BLOCK)) return -1;

    /* Extract current block permissions */
    int old_perms = desc_to_perms(*l2_entry);

    /* Allocate a 4KB page for the L3 table (512 entries × 8 bytes) */
    uint64_t l3_gpa = pt_alloc_page(g);
    if (!l3_gpa) return -1;

    uint64_t *l3 = pt_at(g, l3_gpa);

    /* Fill all 512 L3 entries with 4KB page descriptors inheriting
     * the block's original permissions. Each page covers 4KB of the
     * 2MB block's address range. */
    uint64_t block_ipa = base + block_start;
    for (int i = 0; i < 512; i++) {
        l3[i] = make_page_desc(block_ipa + (uint64_t)i * PAGE_SIZE, old_perms);
    }

    /* Replace the L2 block descriptor with a table descriptor pointing
     * to the new L3 table. Format: bits[1:0]=11, bits[47:12]=L3 IPA */
    *l2_entry = (base + l3_gpa) | PT_VALID | PT_TABLE;

    g->need_tlbi = 1;
    return 0;
}

int guest_invalidate_ptes(guest_t *g, uint64_t start, uint64_t end) {
    uint64_t base = g->ipa_base;

    /* Page-align the range */
    start = start & ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start; addr < end; ) {
        uint64_t *l2_entry = find_l2_entry(g, addr);
        if (!l2_entry) {
            /* No L2 entry — already unmapped, skip this 2MB block */
            addr = ALIGN_2MB_UP(addr + 1);
            continue;
        }

        uint64_t block_start = ALIGN_2MB_DOWN(addr);
        uint64_t block_end = block_start + BLOCK_2MB;

        /* Not mapped at all: skip */
        if (!(*l2_entry & 1)) {
            addr = block_end;
            continue;
        }

        /* Check if this is a 2MB block or already an L3 table */
        if ((*l2_entry & 3) == 1) {
            /* 2MB block descriptor */
            if (start <= block_start && end >= block_end) {
                /* Invalidating the entire 2MB block: clear the L2 entry */
                *l2_entry = 0;
                g->need_tlbi = 1;
                addr = block_end;
                continue;
            }

            /* Partial invalidation within a 2MB block: split first,
             * then invalidate individual L3 pages below. */
            if (guest_split_block(g, block_start) < 0) return -1;
        }

        /* L3 table: invalidate individual 4KB page descriptors */
        uint64_t l3_ipa = *l2_entry & 0xFFFFFFFFF000ULL;
        uint64_t *l3 = pt_at(g, l3_ipa - base);

        uint64_t page_start = (addr > block_start) ? addr : block_start;
        uint64_t page_end = (end < block_end) ? end : block_end;

        for (uint64_t pa = page_start; pa < page_end; pa += PAGE_SIZE) {
            unsigned l3_idx = (unsigned)(((base + pa) % BLOCK_2MB) / PAGE_SIZE);
            l3[l3_idx] = 0;  /* Invalid descriptor */
        }

        g->need_tlbi = 1;
        addr = page_end;
    }

    return 0;
}

int guest_update_perms(guest_t *g, uint64_t start, uint64_t end, int perms) {
    uint64_t base = g->ipa_base;

    /* Page-align the range */
    start = start & ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start; addr < end; ) {
        uint64_t *l2_entry = find_l2_entry(g, addr);
        if (!l2_entry) {
            /* Skip unmapped 2MB blocks */
            addr = ALIGN_2MB_UP(addr + 1);
            continue;
        }

        uint64_t block_start = ALIGN_2MB_DOWN(addr);
        uint64_t block_end = block_start + BLOCK_2MB;

        /* Not mapped at all: skip */
        if (!(*l2_entry & 1)) {
            addr = block_end;
            continue;
        }

        /* Check if this is a 2MB block or already an L3 table */
        if ((*l2_entry & 3) == 1) {
            /* 2MB block descriptor */
            int old_perms = desc_to_perms(*l2_entry);

            /* If we're updating the entire 2MB block and permissions
             * actually differ, just rewrite the block descriptor. */
            if (start <= block_start && end >= block_end) {
                if (old_perms != perms) {
                    uint64_t ipa = base + block_start;
                    *l2_entry = make_block_desc(ipa, perms);
                    g->need_tlbi = 1;
                }
                addr = block_end;
                continue;
            }

            /* Partial update: split the 2MB block into L3 pages first,
             * then fall through to update individual pages below. */
            if (old_perms != perms) {
                if (guest_split_block(g, block_start) < 0) return -1;
            } else {
                /* Same permissions — no change needed */
                addr = block_end;
                continue;
            }
        }

        /* L3 table: update individual 4KB page descriptors */
        uint64_t l3_ipa = *l2_entry & 0xFFFFFFFFF000ULL;
        uint64_t *l3 = pt_at(g, l3_ipa - base);

        /* Update pages within this 2MB block that fall in [start, end) */
        uint64_t page_start = (addr > block_start) ? addr : block_start;
        uint64_t page_end = (end < block_end) ? end : block_end;

        for (uint64_t pa = page_start; pa < page_end; pa += PAGE_SIZE) {
            unsigned l3_idx = (unsigned)(((base + pa) % BLOCK_2MB) / PAGE_SIZE);
            uint64_t page_ipa = base + pa;
            l3[l3_idx] = make_page_desc(page_ipa, perms);
        }

        g->need_tlbi = 1;
        addr = page_end;
    }

    return 0;
}

