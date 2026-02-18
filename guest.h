/* guest.h — Guest memory management for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provides identity-mapped guest physical memory (GVA == GPA == offset into
 * host buffer). A 4GB address space is reserved via mmap(MAP_ANON) (macOS
 * demand-pages physical memory on first touch). The slab is mapped RWX to
 * Hypervisor.framework; fine-grained permissions are enforced by the guest's
 * own page tables built from mem_region descriptors. Page tables can be
 * extended at runtime (e.g. when mmap/brk grows beyond initial mappings).
 */
#ifndef GUEST_H
#define GUEST_H

#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include <stddef.h>

/* ---------- Memory layout constants ---------- */
#define GUEST_MEM_SIZE       0x100000000ULL  /* 4GB total guest address space */
#define PT_POOL_BASE         0x00010000ULL   /* Page table pool start */
#define PT_POOL_END          0x00100000ULL   /* Page table pool end (960KB) */
#define SHIM_BASE            0x00100000ULL   /* Shim code (2MB block, RX) */
#define SHIM_DATA_BASE       0x00200000ULL   /* Shim stack/data (2MB block, RW) */
#define ELF_DEFAULT_BASE     0x00400000ULL   /* Typical ELF load base */
#define BRK_BASE_DEFAULT     0x01000000ULL   /* Default brk start (16MB) */
#define STACK_TOP            0x08000000ULL   /* Stack grows down from here */
#define STACK_BASE           0x07E00000ULL   /* Bottom of 2MB stack block */
#define MMAP_BASE            0x10000000ULL   /* mmap region start */
#define MMAP_INITIAL_END     0x20000000ULL   /* Initial pre-mapped mmap region (512MB) */
#define MMAP_END             0x100000000ULL  /* Max mmap region end (4GB) */
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

/* ---------- Guest state ---------- */
typedef struct {
    void       *host_base;    /* Host pointer to allocated guest memory */
    uint64_t    guest_size;   /* Total size (GUEST_MEM_SIZE) */
    uint64_t    ipa_base;     /* IPA base for hv_vm_map (GUEST_IPA_BASE) */
    uint64_t    pt_pool_next; /* Next free page table page in pool */
    uint64_t    brk_base;     /* Initial brk (set after ELF load) */
    uint64_t    brk_current;  /* Current brk position */
    uint64_t    mmap_next;    /* Next available mmap address */
    uint64_t    mmap_end;     /* Current page-table-covered mmap limit */
    uint64_t    ttbr0;        /* TTBR0 value (IPA of L0 page table) */
    int         need_tlbi;    /* Signal shim to flush TLB after page table changes */
    hv_vcpu_t   vcpu;         /* vCPU handle */
    hv_vcpu_exit_t *exit;     /* vCPU exit info */
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

#endif /* GUEST_H */
