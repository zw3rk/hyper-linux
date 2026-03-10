/* guest.h — Guest memory management for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provides identity-mapped guest physical memory (GVA == GPA == offset into
 * host buffer). Buffer size is determined by the VM's configured IPA width:
 *   - Native aarch64 on M2 (36-bit IPA): 64GB
 *   - Native aarch64 on M3+ (40-bit IPA): 1TB
 *   - Rosetta x86_64 on any hardware (48-bit IPA, capped at 40): 1TB
 * Reserved via mmap(MAP_ANON); macOS demand-pages physical memory on first
 * touch, so unused pages cost nothing. The slab is mapped RWX to
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
#define GUEST_MEM_SIZE_36BIT 0x1000000000ULL     /* 64GB: 36-bit IPA (HVF default) */
#define GUEST_MEM_SIZE_40BIT 0x10000000000ULL    /* 1TB: 40-bit IPA (macOS 15+) */
#define GUEST_MEM_SIZE_DEFAULT GUEST_MEM_SIZE_36BIT /* Fallback if IPA query fails */

/* Rosetta Linux binary: static aarch64-linux ELF that JIT-translates x86_64
 * instructions to ARM64. Linked at 128TB (ET_EXEC, not relocatable). When
 * running x86_64-linux binaries, rosetta is loaded at this fixed address
 * and makes ARM64 Linux syscalls that hl handles transparently. */
#define ROSETTA_IPA_BASE     0x800000000000ULL   /* 128TB: rosetta link address */
#define ROSETTA_PATH         "/Library/Apple/usr/libexec/oah/RosettaLinux/rosetta"
#define PT_POOL_BASE         0x00010000ULL   /* Page table pool start */
#define PT_POOL_END          0x00100000ULL   /* Page table pool end (960KB) */
#define SHIM_BASE            0x00100000ULL   /* Shim code (2MB block, RX) */
#define SHIM_DATA_BASE       0x00200000ULL   /* Shim stack/data (2MB block, RW) */
#define ELF_DEFAULT_BASE     0x00400000ULL   /* Typical ELF load base */
#define PIE_LOAD_BASE        0x00400000ULL   /* PIE (ET_DYN) executable base (4MB) */
#define BRK_BASE_DEFAULT     0x01000000ULL   /* Default brk start (16MB) */
#define STACK_SIZE           0x00800000ULL   /* 8MB stack (4×2MB blocks).
                                              * macOS demand-pages HVF backing memory, so
                                              * unused stack pages consume no host RAM. */
#define STACK_TOP_DEFAULT    0x08000000ULL   /* Default stack top (128MB) — used when
                                              * brk_start is below this.  Otherwise stack
                                              * is placed dynamically above brk. */
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

/* ---------- Kernel VA space (TTBR1) ---------- */
/* Rosetta uses MAP_FIXED at kernel-space addresses (bit 63 set, e.g.,
 * 0xFFFFFFFFFFFFA000) for internal allocations: file mappings, JIT code
 * cache (128MB), thread stacks, etc. In a real VZ VM, rosetta runs at
 * EL1 with full 64-bit VA. We emulate this via TTBR1 page tables that
 * map the top 256MB of VA space to a dedicated host buffer.
 *
 * All rosetta kernel-space addresses observed fall within the last 256MB:
 *   0xFFFFFFFFF0000000 – 0xFFFFFFFFFFFFFFFF
 * This maps linearly to a GPA range within the primary buffer via TTBR1
 * L2 block descriptors. No separate hv_vm_map is needed — the primary
 * buffer's Stage-2 mapping already covers the GPA range. */
#define KBUF_VA_BASE    0xFFFFFFFFF0000000ULL  /* Bottom of 256MB kernel window */
#define KBUF_SIZE       0x10000000ULL          /* 256MB */

/* User VA mirror of the kbuf region. Bits 47:0 match KBUF_VA_BASE.
 * Rosetta's tagged pointer system (TaggedPointer.h) extracts pointers
 * via `value & 0x0000FFFFFFFFFFFF`, stripping bits 63:48 (the tag).
 * When a kernel VA pointer (0xFFFF...) is stored via set_pointer, the
 * BFI instruction overwrites bits 63:48 with the tag. On extraction,
 * get_pointer returns the bits-47:0 version — which is this user VA.
 * By mapping the SAME physical memory at both kernel VA (TTBR1) and
 * user VA (TTBR0), the extracted pointer accesses the correct memory. */
#define KBUF_USER_VA    (KBUF_VA_BASE & 0x0000FFFFFFFFFFFFULL)  /* 0x0000FFFFF0000000 */

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

/* A contiguous region of guest memory to be mapped in page tables.
 * For identity-mapped regions (normal case): va_base=0 means VA==GPA.
 * For non-identity regions (rosetta): va_base specifies the virtual
 * address that maps to gpa_start. Page table walker uses va_base for
 * L0/L1/L2 index computation, gpa_start for block descriptor output. */
typedef struct {
    uint64_t gpa_start;  /* Output IPA/GPA (2MB aligned) */
    uint64_t gpa_end;    /* Output IPA/GPA end (exclusive, 2MB aligned) */
    uint64_t va_base;    /* VA start for this region (0 = same as gpa_start) */
    int      perms;      /* MEM_PERM_* flags */
} mem_region_t;

/* ---------- VA alias mapping ---------- */

/* A virtual-address alias for non-identity-mapped guest memory. Used when
 * guest code references high virtual addresses (e.g., rosetta at 128TB)
 * that are mapped by page tables to low GPAs in the primary buffer.
 * The CPU's MMU handles VA→IPA translation for instruction fetch and
 * data access, but syscall handlers (guest_ptr/read/write) need host-side
 * resolution: VA → host pointer. Each alias maps a VA range to a GPA
 * offset within host_base. */
typedef struct {
    uint64_t  va_start;     /* Guest virtual address start */
    uint64_t  gpa_start;    /* GPA offset in primary buffer (host_base+gpa) */
    uint64_t  size;         /* Region size in bytes */
} guest_va_alias_t;

#define GUEST_MAX_ALIASES 16

/* ---------- Overflow segments ---------- */

/* Overflow segment: additional hv_vm_map'd host buffer for high-VA
 * allocations when the primary buffer's RW region is exhausted.
 * On M2 (max_ipa=36), the primary buffer is limited to 64GB by HVF,
 * but rosetta's JIT allocates many 2MB blocks for high-VA regions
 * (slab at 240TB, PIE at 85TB, thread stacks). Each high-VA 2MB block
 * consumes a 2MB GPA from the primary buffer's mmap RW pool (48GB usable).
 * Large x86_64 binaries exhaust this pool. Overflow segments provide
 * additional GPA space mapped at IPAs beyond the primary buffer. */
typedef struct {
    void     *host_base;   /* Host mmap'd buffer */
    uint64_t  ipa_start;   /* IPA where this segment is mapped */
    uint64_t  size;        /* Total segment size */
    uint64_t  next;        /* Bump allocator: next free offset */
} guest_overflow_t;

#define GUEST_MAX_OVERFLOW  4
#define GUEST_OVERFLOW_SIZE (1ULL * 1024 * 1024 * 1024)  /* 1GB per segment */

/* ---------- Semantic region tracking ---------- */

/* Maximum number of tracked memory regions (heap/stack/mmap/ELF/etc.).
 * GHC RTS creates many regions via mmap/mprotect on its PROT_NONE
 * reservation; 1024 provides ample headroom. */
#define GUEST_MAX_REGIONS 1024

/* Preannounced regions appear in /proc/self/maps but NOT in the main
 * regions[] array. Used in rosetta mode to advertise x86_64 binary LOAD
 * segments before rosetta maps them via MAP_FIXED_NOREPLACE. Rosetta reads
 * /proc/self/maps once at startup and caches the code map for JIT target
 * validation; without these entries, indirect call targets (function
 * pointers) fail with "BasicBlock requested for unrecognized address".
 * Kept separate from regions[] to avoid -EEXIST on rosetta's NOREPLACE. */
#define GUEST_MAX_PREANNOUNCED 16

/* A semantic memory region tracked for munmap/mprotect and /proc/self/maps.
 * Distinct from mem_region_t which is used purely for page table construction.
 * Regions are kept sorted by start address in guest_t.regions[]. */
typedef struct {
    uint64_t start;       /* GPA start for gap-finder (page-aligned) */
    uint64_t end;         /* GPA end (exclusive, page-aligned) */
    int      prot;        /* LINUX_PROT_* flags */
    int      flags;       /* LINUX_MAP_* flags (for /proc/self/maps display) */
    uint64_t offset;      /* File offset (for /proc/self/maps display) */
    uint64_t display_va;  /* Non-zero: /proc/self/maps shows this VA instead
                           * of start. Used for high-VA mmaps (e.g., rosetta's
                           * slab at 240TB) backed by GPA in primary buffer. */
    uint64_t display_end; /* Non-zero: overrides the computed display end
                           * (display_va + (end - start)). Used when the
                           * backing GPA is larger than the actual mmap (e.g.,
                           * 2MB-aligned GPA for a non-2MB-aligned mmap). */
    char     name[64];    /* Label: "[heap]", "[stack]", ELF path, etc. */
} guest_region_t;

/* ---------- Guest state ---------- */
typedef struct {
    void       *host_base;    /* Host pointer to allocated guest memory */
    int         shm_fd;       /* File fd backing host_base for COW fork (-1 if MAP_ANON) */
    uint64_t    guest_size;   /* Total size (determined by IPA capacity) */
    uint64_t    ipa_base;     /* IPA base for hv_vm_map (GUEST_IPA_BASE) */
    uint64_t    mmap_limit;   /* Max mmap address (computed from guest_size) */
    uint64_t    interp_base;  /* Dynamic linker load base (computed from guest_size) */
    uint64_t    pt_pool_next; /* Next free page table page in pool */
    uint64_t    brk_base;     /* Initial brk (set after ELF load) */
    uint64_t    brk_current;  /* Current brk position */
    uint64_t    stack_base;   /* Bottom of stack region (dynamic, above brk) */
    uint64_t    stack_top;    /* Top of stack (stack grows down from here) */
    uint64_t    mmap_next;    /* RW mmap high-water mark (for fork IPC state transfer) */
    uint64_t    mmap_end;     /* Current page-table-covered RW mmap limit */
    uint64_t    mmap_rx_next; /* RX mmap high-water mark (for fork IPC state transfer) */
    uint64_t    mmap_rx_end;  /* Current page-table-covered RX mmap limit */
    uint64_t    ttbr0;        /* TTBR0 value (IPA of L0 page table) */
    uint64_t    ttbr1;        /* TTBR1 value (IPA of L0 page table for kernel VA) */
    uint64_t    kbuf_gpa;     /* GPA of 256MB kernel VA buffer in primary buffer */
    void       *kbuf_base;    /* Host pointer to kbuf (host_base + kbuf_gpa) */
    int         need_tlbi;    /* Signal shim to flush TLB after page table changes */
    hv_vcpu_t   vcpu;         /* vCPU handle */
    hv_vcpu_exit_t *exit;     /* vCPU exit info */
    /* VA alias mappings for non-identity regions (e.g., rosetta at 128TB
     * mapped to low GPA in primary buffer). Used by gva_resolve() for
     * syscall handlers that need to access guest memory by VA. */
    guest_va_alias_t va_aliases[GUEST_MAX_ALIASES];
    int              naliases;
    uint32_t            ipa_bits;  /* IPA bits requested from HVF */
    int                 is_rosetta; /* Non-zero when running x86_64 via rosetta */
    /* Rosetta placement — survives guest_reset() for execve re-setup.
     * Set by rosetta_prepare() on first load; reused on subsequent loads
     * (e.g. execve of another x86_64 binary) so segments land at the
     * same GPA and the existing VA aliases remain valid. */
    uint64_t  rosetta_guest_base;  /* GPA in primary buffer where rosetta is loaded */
    uint64_t  rosetta_va_base;     /* High VA start (e.g. 0x800000000000) */
    uint64_t  rosetta_size;        /* 2MB-aligned total rosetta span */
    /* Semantic region tracking for munmap/mprotect/proc-self-maps */
    guest_region_t regions[GUEST_MAX_REGIONS];
    int            nregions;  /* Number of active regions */
    /* Preannounced regions for /proc/self/maps (rosetta mode only).
     * See GUEST_MAX_PREANNOUNCED comment above. */
    guest_region_t preannounced[GUEST_MAX_PREANNOUNCED];
    int            npreannounced;
    int            verbose;       /* Non-zero: print debug diagnostics to stderr */
    /* Overflow segments for high-VA GPA exhaustion (rosetta on M2).
     * When find_free_gap() can't allocate from the primary buffer's
     * mmap RW region, new 2MB blocks come from overflow segments
     * mapped at IPAs beyond guest_size. */
    guest_overflow_t overflow[GUEST_MAX_OVERFLOW];
    int              noverflow;
    uint64_t         overflow_ipa_next; /* Next IPA for new overflow segment */
} guest_t;

/* Convert a guest offset (0-based) to an IPA/VA (ipa_base + offset) */
static inline uint64_t guest_ipa(const guest_t *g, uint64_t offset) {
    return g->ipa_base + offset;
}

/* ---------- API ---------- */

/* Allocate guest memory, create VM, map to hypervisor.
 * size: primary buffer size (0 = auto-detect from IPA capacity).
 * ipa_bits: IPA width for HVF VM (0 = auto-detect).
 * Returns 0 on success, -1 on failure. */
int guest_init(guest_t *g, uint64_t size, uint32_t ipa_bits);

/* Initialize guest from a POSIX shared memory fd (COW fork path).
 * Maps shm_fd MAP_PRIVATE (copy-on-write), creates HVF VM, maps to
 * hypervisor. The child gets an instant COW snapshot of parent's guest
 * memory without copying. shm_fd is closed after mapping.
 * Returns 0 on success, -1 on failure. */
int guest_init_from_shm(guest_t *g, int shm_fd, uint64_t size,
                         uint32_t ipa_bits);

/* Register a VA alias for a non-identity-mapped region. Allows syscall
 * handlers (guest_ptr/read/write) to resolve high virtual addresses
 * (e.g., rosetta at 128TB) to the correct host pointer within host_base.
 * va_start: guest virtual address range start
 * gpa_start: GPA offset where data lives in primary buffer
 * size: region size in bytes
 * Returns 0 on success, -1 if alias table is full. */
int guest_add_va_alias(guest_t *g, uint64_t va_start,
                       uint64_t gpa_start, uint64_t size);

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

/* Initialize kernel VA space (TTBR1) for rosetta's kernel-space mmaps.
 * Uses the primary buffer at `kbuf_gpa` (must be within guest_size, 2MB
 * aligned) and builds TTBR1 page tables (L0→L1→L2) mapping the top 256MB
 * of VA space to that GPA range. Sets g->ttbr1, g->kbuf_gpa, g->kbuf_base.
 * Returns 0 on success, -1 on failure. */
int guest_init_kbuf(guest_t *g, uint64_t kbuf_gpa);

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

/* Kernel VA (TTBR1) W^X support: split a 2MB block and update permissions
 * for pages in the kernel buffer region. Used by the HVC #9 handler when
 * rosetta's JIT needs permission toggling on kernel VA pages. */
int guest_kbuf_split_block(guest_t *g, uint64_t block_va);
int guest_kbuf_update_perms(guest_t *g, uint64_t start, uint64_t end, int perms);
int guest_kbuf_invalidate_ptes(guest_t *g, uint64_t start, uint64_t end);

/* Create non-identity page table entries mapping a VA range to a GPA range.
 * Used for high-VA MAP_FIXED (e.g., rosetta slab allocator at 240TB).
 * L0/L1/L2 indices are derived from va_start; block descriptor output IPAs
 * come from gpa_start. Both va_start and gpa_start must be 2MB-aligned.
 * Sets g->need_tlbi = 1. Returns 0 on success, -1 on failure. */
int guest_map_va_range(guest_t *g, uint64_t va_start, uint64_t va_end,
                       uint64_t gpa_start, int perms);

/* Allocate a 2MB-aligned GPA block from overflow segments.
 * Creates a new overflow segment if needed (1GB, mapped via hv_vm_map
 * at an IPA beyond the primary buffer). Returns the allocated IPA,
 * or UINT64_MAX on failure. */
uint64_t guest_overflow_alloc(guest_t *g);

/* Translate a host pointer back to a GPA, checking both the primary
 * buffer and overflow segments. Returns 0 on success, -1 if the
 * pointer doesn't belong to any known region. */
int guest_host_to_gpa(const guest_t *g, const void *ptr, uint64_t *out_gpa);

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

/* Add a preannounced region (appears in /proc/self/maps but is NOT checked
 * by MAP_FIXED_NOREPLACE). Used for x86_64/rosetta mode where the binary
 * is pre-mapped by hl but Rosetta still needs to do its own MAP_FIXED.
 * Sorted by start address; shadowed by actual regions[] in maps output. */
int guest_preannounce(guest_t *g, uint64_t start, uint64_t end,
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
