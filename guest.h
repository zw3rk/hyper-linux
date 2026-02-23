/* guest.h — Guest memory management for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provides identity-mapped guest physical memory (GVA == GPA == offset into
 * host buffer). A 64GB address space is reserved via mmap(MAP_ANON) (macOS
 * demand-pages physical memory on first touch, so unused pages cost nothing).
 * The slab is mapped RWX to Hypervisor.framework; fine-grained permissions
 * are enforced by the guest's own page tables built from mem_region
 * descriptors. Page tables can be extended at runtime (e.g. when mmap/brk
 * grows beyond initial mappings).
 */
#ifndef GUEST_H
#define GUEST_H

#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include <stddef.h>

/* ---------- Memory layout constants ---------- */
#define GUEST_MEM_SIZE_36BIT 0x1000000000ULL  /* 64GB: 36-bit IPA (HVF default) */
#define GUEST_MEM_SIZE_40BIT 0x10000000000ULL /* 1TB: 40-bit IPA (macOS 15+) */
#define GUEST_MEM_SIZE_DEFAULT GUEST_MEM_SIZE_36BIT /* Fallback if IPA query fails */
#define PT_POOL_BASE         0x00010000ULL   /* Page table pool start */
#define PT_POOL_END          0x00100000ULL   /* Page table pool end (960KB) */
#define SHIM_BASE            0x00100000ULL   /* Shim code (2MB block, RX) */
#define SHIM_DATA_BASE       0x00200000ULL   /* Shim stack/data (2MB block, RW) */
#define ELF_DEFAULT_BASE     0x00400000ULL   /* Typical ELF load base */
#define PIE_LOAD_BASE        0x00400000ULL   /* PIE (ET_DYN) executable base (4MB) */
#define BRK_BASE_DEFAULT     0x01000000ULL   /* Default brk start (16MB) */
#define STACK_TOP            0x08000000ULL   /* Stack grows down from here */
#define STACK_BASE           0x07E00000ULL   /* Bottom of 2MB stack block */
#define STACK_GUARD_SIZE     0x00001000ULL   /* 4KB guard page at bottom of stack */
#define MMAP_RX_BASE         0x10000000ULL   /* mmap RX region start (for PROT_EXEC).
                                              * Below 8GB — only code goes here, not
                                              * subject to GHC's minimumAddress check. */
#define MMAP_RX_INITIAL_END  0x20000000ULL   /* Initial pre-mapped mmap RX end (512MB) */
#define MMAP_BASE            0x200000000ULL  /* mmap RW region start (8GB). Placed high to
                                              * match real Linux mmap address space layout.
                                              * Programs may assume mmap returns addresses
                                              * well above text/data/brk (e.g. for pointer
                                              * tagging or address-space partitioning). */
#define MMAP_INITIAL_END     0x210000000ULL  /* Initial pre-mapped mmap RW end (8.25GB) */
#define MMAP_END_DEFAULT     0xE00000000ULL  /* Default max mmap end for 36-bit IPA (56GB).
                                              * PROT_NONE mmaps beyond the page-table-covered
                                              * region cost nothing (no page tables, no zeroing). */
#define INTERP_LOAD_DEFAULT  0xF00000000ULL  /* Default dynamic linker base for 36-bit IPA (60GB) */
#define BLOCK_2MB            (2ULL * 1024 * 1024)

/* IPA base: guest memory is mapped at this IPA in the hypervisor.
 * All guest physical addresses = GUEST_IPA_BASE + offset.
 * Must be 0 so that guest virtual addresses match ELF link addresses
 * (e.g. 0x400000). A non-zero IPA base would require all ELF binaries
 * to be linked at IPA_BASE+vaddr, which is impractical. */
#define GUEST_IPA_BASE   0x0ULL

/* ---------- Page table attributes ---------- */
/* Memory region permission flags */
#define MEM_PERM_R   (1 << 0)
#define MEM_PERM_W   (1 << 1)
#define MEM_PERM_X   (1 << 2)
#define MEM_PERM_RX  (MEM_PERM_R | MEM_PERM_X)
#define MEM_PERM_RW  (MEM_PERM_R | MEM_PERM_W)
#define MEM_PERM_RWX (MEM_PERM_R | MEM_PERM_W | MEM_PERM_X)

/* A contiguous region of guest memory to be mapped in page tables */
typedef struct {
    uint64_t gpa_start;  /* Guest physical address (2MB aligned) */
    uint64_t gpa_end;    /* End address (exclusive, 2MB aligned) */
    int      perms;      /* MEM_PERM_* flags */
} mem_region_t;

/* ---------- Semantic region tracking ---------- */

/* Maximum number of tracked memory regions (heap/stack/mmap/ELF/etc.).
 * GHC RTS creates many regions via mmap/mprotect on its PROT_NONE
 * reservation; 1024 provides ample headroom. */
#define GUEST_MAX_REGIONS 1024

/* A semantic memory region tracked for munmap/mprotect and /proc/self/maps.
 * Distinct from mem_region_t which is used purely for page table construction.
 * Regions are kept sorted by start address in guest_t.regions[]. */
typedef struct {
    uint64_t start;       /* GVA start (page-aligned) */
    uint64_t end;         /* GVA end (exclusive, page-aligned) */
    int      prot;        /* LINUX_PROT_* flags */
    int      flags;       /* LINUX_MAP_* flags (for /proc/self/maps display) */
    uint64_t offset;      /* File offset (for /proc/self/maps display) */
    char     name[64];    /* Label: "[heap]", "[stack]", ELF path, etc. */
} guest_region_t;

/* ---------- Guest state ---------- */
typedef struct {
    void       *host_base;    /* Host pointer to allocated guest memory */
    uint64_t    guest_size;   /* Total size (determined by IPA capacity) */
    uint64_t    ipa_base;     /* IPA base for hv_vm_map (GUEST_IPA_BASE) */
    uint64_t    mmap_limit;   /* Max mmap address (computed from guest_size) */
    uint64_t    interp_base;  /* Dynamic linker load base (computed from guest_size) */
    uint64_t    pt_pool_next; /* Next free page table page in pool */
    uint64_t    brk_base;     /* Initial brk (set after ELF load) */
    uint64_t    brk_current;  /* Current brk position */
    uint64_t    mmap_next;    /* RW mmap high-water mark (for fork IPC state transfer) */
    uint64_t    mmap_end;     /* Current page-table-covered RW mmap limit */
    uint64_t    mmap_rx_next; /* RX mmap high-water mark (for fork IPC state transfer) */
    uint64_t    mmap_rx_end;  /* Current page-table-covered RX mmap limit */
    uint64_t    ttbr0;        /* TTBR0 value (IPA of L0 page table) */
    int         need_tlbi;    /* Signal shim to flush TLB after page table changes */
    hv_vcpu_t   vcpu;         /* vCPU handle */
    hv_vcpu_exit_t *exit;     /* vCPU exit info */
    /* Semantic region tracking for munmap/mprotect/proc-self-maps */
    guest_region_t regions[GUEST_MAX_REGIONS];
    int            nregions;  /* Number of active regions */
} guest_t;

/* Convert a guest offset (0-based) to an IPA/VA (ipa_base + offset) */
static inline uint64_t guest_ipa(const guest_t *g, uint64_t offset) {
    return g->ipa_base + offset;
}

/* ---------- API ---------- */

/* Allocate guest memory, create VM, map to hypervisor.
 * Returns 0 on success, -1 on failure. */
int guest_init(guest_t *g, uint64_t size);

/* Tear down VM and free guest memory. */
void guest_destroy(guest_t *g);

/* Get a host pointer for a guest virtual address.
 * Returns NULL if gva is out of bounds. */
void *guest_ptr(const guest_t *g, uint64_t gva);

/* Bounds-checked copy from guest memory to host buffer.
 * Returns 0 on success, -1 if out of bounds. */
int guest_read(const guest_t *g, uint64_t gva, void *dst, size_t len);

/* Bounds-checked copy from host buffer into guest memory.
 * Returns 0 on success, -1 if out of bounds. */
int guest_write(guest_t *g, uint64_t gva, const void *src, size_t len);

/* Read a null-terminated string from guest memory.
 * Copies up to max-1 bytes + NUL into dst.
 * Returns string length or -1 if out of bounds / unterminated. */
int guest_read_str(const guest_t *g, uint64_t gva, char *dst, size_t max);

/* Build L0->L1->L2 page tables from an array of memory regions.
 * Uses 2MB block descriptors. Returns the TTBR0 value (GPA of L0 table),
 * or 0 on failure. */
uint64_t guest_build_page_tables(guest_t *g, const mem_region_t *regions, int n);

/* Extend page tables to cover a new address range [start, end) with 2MB
 * block descriptors. Reuses the existing L0→L1 table structure and
 * allocates new L2 tables as needed. Sets g->need_tlbi = 1.
 * Returns 0 on success, -1 on failure. */
int guest_extend_page_tables(guest_t *g, uint64_t start, uint64_t end, int perms);

/* Split a 2MB block descriptor into 512 × 4KB L3 page descriptors.
 * block_gpa must be within a currently-mapped 2MB block. The block's
 * permissions are inherited by all 512 page entries. If the block is
 * already split (L2 entry is a table descriptor), this is a no-op.
 * Sets g->need_tlbi = 1. Returns 0 on success, -1 on failure. */
int guest_split_block(guest_t *g, uint64_t block_gpa);

/* Invalidate page table entries for the range [start, end).
 * Sets L2 block descriptors and L3 page descriptors to 0 (invalid),
 * causing translation faults on access. Used when mprotect sets
 * PROT_NONE — the correct behavior is for the guest to fault.
 * If a 2MB block is only partially invalidated, the block is split
 * into L3 pages first (preserving the non-invalidated pages).
 * Sets g->need_tlbi = 1. Returns 0 on success, -1 on failure. */
int guest_invalidate_ptes(guest_t *g, uint64_t start, uint64_t end);

/* Update page table permissions for the range [start, end).
 * If a 2MB block needs mixed permissions (only part of it is being
 * updated), the block is automatically split into 4KB L3 pages first.
 * If the entire 2MB block is being updated, the block descriptor is
 * modified in place without splitting.
 * perms is a MEM_PERM_R/W/X combination. Sets g->need_tlbi = 1.
 * Returns 0 on success, -1 on failure. */
int guest_update_perms(guest_t *g, uint64_t start, uint64_t end, int perms);

/* Reset guest memory for execve. Zeros ELF, brk, stack, mmap regions and
 * resets page table pool, brk, and mmap allocation state. Preserves the
 * host_base mapping and VM/vCPU handles. */
void guest_reset(guest_t *g);

/* A used memory region for fork state transfer */
typedef struct {
    uint64_t offset;  /* Offset from host_base (0-based) */
    uint64_t size;    /* Size in bytes */
} used_region_t;

/* Enumerate used memory regions for fork state transfer.
 * Writes up to max entries into out[]. Returns the count written.
 * shim_size is the shim binary size (needed to determine shim region). */
int guest_get_used_regions(const guest_t *g, unsigned int shim_size,
                           used_region_t *out, int max);

/* ---------- Semantic region tracking API ---------- */

/* Add a region to the sorted tracking array. Overlapping regions are NOT
 * merged — each call adds a distinct entry. If the array is full the
 * call is silently ignored (non-fatal; only /proc/self/maps loses data).
 * Returns 0 on success, -1 if the region table is full. */
int guest_region_add(guest_t *g, uint64_t start, uint64_t end,
                     int prot, int flags, uint64_t offset,
                     const char *name);

/* Remove all region coverage in [start, end). Regions fully contained are
 * deleted; partially overlapping regions are trimmed or split. */
void guest_region_remove(guest_t *g, uint64_t start, uint64_t end);

/* Find the region containing addr. Returns a pointer to the region (inside
 * the guest_t regions array) or NULL if addr is not in any tracked region. */
const guest_region_t *guest_region_find(const guest_t *g, uint64_t addr);

/* Update protection bits for all region coverage in [start, end).
 * Splits regions at boundaries as needed. */
void guest_region_set_prot(guest_t *g, uint64_t start, uint64_t end, int prot);

/* Clear all tracked regions. Used by execve before re-adding new regions. */
void guest_region_clear(guest_t *g);

#endif /* GUEST_H */
