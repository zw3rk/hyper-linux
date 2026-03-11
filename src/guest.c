/* guest.c — Guest memory management for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Identity-mapped guest memory: GVA == GPA == offset into host_base.
 * The guest address space size is determined by the VM's configured IPA
 * width (capped at 40-bit = 1TB): 64GB for native aarch64 on M2 (36-bit),
 * 1TB for M3+ (40-bit) or rosetta mode on any hardware. Reserved via
 * mmap(MAP_ANON); macOS demand-pages physical memory on first touch, so
 * only used pages consume RAM. The slab is mapped RWX to
 * Hypervisor.framework. The guest's own page tables
 * (built here) enforce per-region permissions using 2MB block descriptors,
 * which are mandatory for transparent misaligned access. Page tables can be
 * extended at runtime via guest_extend_page_tables().
 *
 * PROT_NONE mappings in the primary address space (used by GHC RTS for
 * heap reservation) do NOT get page table entries — the translation
 * fault is the correct behavior. High-VA PROT_NONE reservations (used
 * by Rosetta for PIE binary loading) DO get minimal PTEs (MEM_PERM_R)
 * to avoid guest_ptr() returning NULL during MAP_FIXED overlay.
 * When mprotect changes an accessible region to PROT_NONE,
 * guest_invalidate_ptes() removes existing page table entries.
 * Page tables are created on demand when mprotect changes PROT_NONE
 * to an accessible permission.
 *
 * Page table format: AArch64 4KB granule, up to 4-level:
 *   L0 entry covers 512GB — multiple entries for >512GB (rosetta at 128TB)
 *   L1 entry covers 1GB  — either block or table pointing to L2
 *   L2 entry covers 2MB  — block descriptors with final permissions
 *   L3 entry covers 4KB  — optional, created by guest_split_block() for
 *                           mixed permissions within a 2MB block (W^X)
 */
#include "guest.h"
#include "syscall_internal.h"  /* hl_verbose */
#include "thread.h"            /* thread_destroy_all_vcpus */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------- Page table descriptor bits ---------- */
#define PT_VALID       (1ULL << 0)
#define PT_TABLE       (1ULL << 1)  /* Table descriptor (L0/L1/L2) */
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
/* BLOCK_2MB defined in guest.h — reuse it */
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
 * Acquires pt_lock internally. Caller typically holds mmap_lock. */
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
        if (hl_verbose)
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

int guest_init(guest_t *g, uint64_t size, uint32_t ipa_bits) {
    memset(g, 0, sizeof(*g));
    g->shm_fd = -1;
    g->ipa_base = GUEST_IPA_BASE;
    g->pt_pool_next = PT_POOL_BASE;
    g->brk_base = BRK_BASE_DEFAULT;
    g->brk_current = BRK_BASE_DEFAULT;
    g->mmap_next = MMAP_BASE;
    g->mmap_rx_next = MMAP_RX_BASE;

    /* Query the maximum IPA size supported by the hardware/kernel.
     * macOS 15+ on Apple Silicon reports 40 bits (1TB). Older versions
     * or fallback yields 36 bits (64GB). Apple Silicon actually supports
     * up to 48-bit IPA despite the API reporting 36 as default — this is
     * needed for rosetta at 0x800000000000 (128TB). */
    uint32_t max_ipa = 0;
    hv_vm_config_get_max_ipa_size(&max_ipa);
    if (max_ipa < 36) max_ipa = 36;

    /* Determine VM IPA width.
     * ipa_bits=0: auto-detect (40-bit on macOS 15+, else 36-bit).
     * ipa_bits>0: use that exact value (e.g., 48 for rosetta support).
     *   Apple Silicon supports 48-bit IPA even when the API reports 36. */
    uint32_t vm_ipa;
    if (ipa_bits > 0)
        vm_ipa = ipa_bits;
    else if (max_ipa >= 40)
        vm_ipa = 40;
    else
        vm_ipa = 36;

    /* Primary buffer size: use the VM's configured IPA width (capped at
     * 40-bit = 1TB). When rosetta is active (vm_ipa=48), this gives 1TB
     * instead of 64GB on M2, providing ~1008GB of mmap backing space for
     * rosetta's high-VA JIT allocations (slab at 240TB, PIE at 85TB, etc.).
     * The hv_vm_map call supports any size up to the VM's IPA width —
     * confirmed by rosetta's own segments mapped at 128TB via hv_vm_map.
     * macOS demand-pages the host reservation, so only touched pages cost
     * physical memory. */
    uint32_t buf_bits = (vm_ipa > 40) ? 40 : vm_ipa;
    uint64_t buf_capacity = 1ULL << buf_bits;
    if (size == 0 || size > buf_capacity)
        size = buf_capacity;
    g->guest_size = size;
    g->ipa_bits = vm_ipa;

    /* Compute dynamic layout limits from primary buffer size.
     * interp_base: last 4GB (dynamic linker load address)
     * mmap_limit:  last 8GB reserved (max mmap RW address)
     * For 64GB:  interp=60GB, mmap_limit=56GB
     * For 1TB:   interp=1020GB, mmap_limit=1016GB */
    g->interp_base = g->guest_size - 0x100000000ULL;
    g->mmap_limit  = g->guest_size - 0x200000000ULL;

    /* Reserve primary address space via mmap(MAP_ANON). macOS demand-pages
     * this: physical pages are allocated only on first touch, so reserving
     * up to 1TB costs nothing until pages are actually used. Do NOT memset
     * — that would touch all pages and defeat demand paging. */
    g->host_base = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
    if (g->host_base == MAP_FAILED) {
        perror("guest: mmap");
        g->host_base = NULL;
        return -1;
    }

    /* Upgrade to file-backed shared memory for COW fork support.
     * mkstemp + unlink + ftruncate + MAP_SHARED|MAP_FIXED replaces the
     * anonymous mapping with file-backed memory at the same host address.
     * At fork time, the parent stays on MAP_SHARED (HVF caches VA→PA, so
     * remapping would cause stale reads) and sends the file fd to the child.
     * The child maps it MAP_PRIVATE, giving it an instant copy-on-write
     * clone of all guest memory.
     *
     * macOS rejects MAP_PRIVATE on shm_open objects (EINVAL), but regular
     * file fds support MAP_SHARED, MAP_PRIVATE, and MAP_PRIVATE|MAP_FIXED
     * correctly. The file is unlinked immediately — the fd keeps it alive.
     * macOS demand-pages file mappings, so untouched pages cost nothing.
     * If any step fails, we silently keep the MAP_ANON mapping and fall
     * back to the IPC region-copy path on fork.
     *
     * Skip for rosetta mode (vm_ipa > max_ipa): rosetta always uses the
     * legacy IPC copy path (fork_ipc.c checks g->is_rosetta), so the
     * file-backed upgrade is pointless and avoids a 1TB ftruncate. */
    if (vm_ipa <= max_ipa) {
        char tmppath[] = "/tmp/hl-XXXXXX";
        int sfd = mkstemp(tmppath);
        if (sfd >= 0) {
            unlink(tmppath);  /* Unlink immediately; fd keeps file alive */
            if (ftruncate(sfd, (off_t)size) == 0) {
                void *p = mmap(g->host_base, size, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_FIXED, sfd, 0);
                if (p != MAP_FAILED) {
                    g->shm_fd = sfd;
                } else {
                    /* MAP_FIXED failed; keep the original MAP_ANON mapping */
                    close(sfd);
                }
            } else {
                close(sfd);
            }
        }
        /* If shm_fd is still -1, we're on MAP_ANON — fork uses IPC copy */
    }

    /* Create Hypervisor VM with the determined IPA width and map the
     * primary slab at GUEST_IPA_BASE.
     *
     * macOS may not release HVF VM resources immediately after
     * hv_vm_destroy(), so rapid sequential VM creation (e.g. running
     * many test binaries) can hit transient resource exhaustion.
     * Retry with linear backoff (500ms intervals, up to 30 attempts =
     * 15 seconds max wait) to handle this gracefully. */
    hv_return_t ret = HV_ERROR;
    for (int attempt = 0; attempt < 30; attempt++) {
        hv_vm_config_t config = hv_vm_config_create();
        hv_vm_config_set_ipa_size(config, vm_ipa);
        ret = hv_vm_create(config);
        os_release(config);
        if (ret == HV_SUCCESS)
            break;
        usleep(500000);  /* 500ms between attempts */
    }
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "guest: hv_vm_create failed: %d (ipa_bits=%u)\n",
                (int)ret, vm_ipa);
        munmap(g->host_base, size);
        g->host_base = NULL;
        if (g->shm_fd >= 0) { close(g->shm_fd); g->shm_fd = -1; }
        return -1;
    }

    ret = hv_vm_map(g->host_base, GUEST_IPA_BASE, size,
                    HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (ret != HV_SUCCESS && buf_bits > max_ipa) {
        /* 1TB primary map failed — fall back to hardware-default buffer.
         * This handles undocumented HVF limits on primary buffer size.
         * Close shm_fd since the fallback uses anonymous memory (the file
         * is no longer mapped to host_base, so COW fork won't work). */
        fprintf(stderr, "guest: hv_vm_map %lluGB failed (%d), "
                "retrying with %u-bit (%lluGB)\n",
                (unsigned long long)(size >> 30), (int)ret,
                max_ipa, 1ULL << (max_ipa - 30));
        munmap(g->host_base, size);
        if (g->shm_fd >= 0) { close(g->shm_fd); g->shm_fd = -1; }
        buf_bits = (max_ipa > 40) ? 40 : max_ipa;
        size = 1ULL << buf_bits;
        g->guest_size = size;
        g->interp_base = size - 0x100000000ULL;
        g->mmap_limit  = size - 0x200000000ULL;
        g->host_base = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_ANON | MAP_PRIVATE, -1, 0);
        if (g->host_base == MAP_FAILED) {
            perror("guest: mmap (fallback)");
            g->host_base = NULL;
            hv_vm_destroy();
            return -1;
        }
        ret = hv_vm_map(g->host_base, GUEST_IPA_BASE, size,
                        HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    }
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "guest: hv_vm_map failed: %d\n", (int)ret);
        hv_vm_destroy();
        munmap(g->host_base, size);
        g->host_base = NULL;
        if (g->shm_fd >= 0) { close(g->shm_fd); g->shm_fd = -1; }
        return -1;
    }

    /* Printed by hl.c after setting g->verbose (guest_init runs before
     * verbose is set, so we can't check g->verbose here). The IPA info
     * is printed via the verbose block in hl.c main() instead. */

    /* Overflow segments start at the IPA just beyond the primary buffer */
    g->overflow_ipa_next = g->guest_size;

    return 0;
}

int guest_init_from_shm(guest_t *g, int shm_fd, uint64_t size,
                         uint32_t ipa_bits) {
    memset(g, 0, sizeof(*g));
    g->shm_fd = -1;  /* Child doesn't own the shm */
    g->ipa_base = GUEST_IPA_BASE;
    g->pt_pool_next = PT_POOL_BASE;
    g->brk_base = BRK_BASE_DEFAULT;
    g->brk_current = BRK_BASE_DEFAULT;
    g->mmap_next = MMAP_BASE;
    g->mmap_rx_next = MMAP_RX_BASE;
    g->guest_size = size;
    g->ipa_bits = ipa_bits;

    /* Compute layout limits (same formula as guest_init) */
    g->interp_base = size - 0x100000000ULL;
    g->mmap_limit  = size - 0x200000000ULL;

    /* Map the shm fd MAP_PRIVATE: copy-on-write semantics. Reads see
     * the parent's frozen snapshot; writes are private to this process.
     * macOS COW is page-granular — only modified pages are duplicated. */
    g->host_base = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE, shm_fd, 0);
    if (g->host_base == MAP_FAILED) {
        perror("guest: mmap shm");
        g->host_base = NULL;
        close(shm_fd);
        return -1;
    }

    /* Close the shm fd — the mapping keeps the pages alive */
    close(shm_fd);

    /* Create HVF VM with the same IPA width as the parent */
    hv_return_t ret = HV_ERROR;
    for (int attempt = 0; attempt < 30; attempt++) {
        hv_vm_config_t config = hv_vm_config_create();
        hv_vm_config_set_ipa_size(config, ipa_bits);
        ret = hv_vm_create(config);
        os_release(config);
        if (ret == HV_SUCCESS)
            break;
        usleep(500000);
    }
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "guest: hv_vm_create (shm) failed: %d\n", (int)ret);
        munmap(g->host_base, size);
        g->host_base = NULL;
        return -1;
    }

    ret = hv_vm_map(g->host_base, GUEST_IPA_BASE, size,
                    HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "guest: hv_vm_map (shm) failed: %d\n", (int)ret);
        hv_vm_destroy();
        munmap(g->host_base, size);
        g->host_base = NULL;
        return -1;
    }

    if (hl_verbose)
        fprintf(stderr, "guest: COW fork: mapped %lluGB from shm "
                "(ipa=%u bits)\n",
                (unsigned long long)(size / (1024ULL * 1024 * 1024)),
                ipa_bits);

    /* Overflow segments start at the IPA just beyond the primary buffer */
    g->overflow_ipa_next = g->guest_size;

    return 0;
}

int guest_init_kbuf(guest_t *g, uint64_t kbuf_gpa) {
    /* Validate: kbuf_gpa must be 2MB-aligned and fit within primary buffer */
    if ((kbuf_gpa & (BLOCK_2MB - 1)) != 0) {
        fprintf(stderr, "guest: kbuf_gpa 0x%llx not 2MB-aligned\n",
                (unsigned long long)kbuf_gpa);
        return -1;
    }
    if (kbuf_gpa + KBUF_SIZE > g->guest_size) {
        fprintf(stderr, "guest: kbuf_gpa 0x%llx + 256MB exceeds guest_size "
                "0x%llx\n", (unsigned long long)kbuf_gpa,
                (unsigned long long)g->guest_size);
        return -1;
    }

    /* The kbuf lives within the primary buffer — no separate mmap or
     * hv_vm_map needed. The primary buffer's Stage-2 mapping (RWX at IPA 0)
     * already covers this GPA range. macOS demand-pages the host buffer,
     * so untouched pages cost nothing. */
    g->kbuf_gpa = kbuf_gpa;
    g->kbuf_base = (uint8_t *)g->host_base + kbuf_gpa;

    /* Build TTBR1 page tables: L0[511] → L1 → L1[511] → L2.
     * L2 entries 384..511 cover the 256MB kernel VA window with 2MB
     * block descriptors. Pages are allocated from the PT pool and
     * live in the primary host_base buffer. */
    uint64_t l0_gpa = pt_alloc_page(g);
    uint64_t l1_gpa = pt_alloc_page(g);
    uint64_t l2_gpa = pt_alloc_page(g);
    if (!l0_gpa || !l1_gpa || !l2_gpa) {
        fprintf(stderr, "guest: kbuf page table alloc failed\n");
        g->kbuf_base = NULL;
        g->kbuf_gpa = 0;
        return -1;
    }

    /* L0: entry 511 → L1 table */
    uint64_t *l0 = pt_at(g, l0_gpa);
    l0[511] = (g->ipa_base + l1_gpa) | PT_VALID | PT_TABLE;

    /* L1: entry 511 → L2 table */
    uint64_t *l1 = pt_at(g, l1_gpa);
    l1[511] = (g->ipa_base + l2_gpa) | PT_VALID | PT_TABLE;

    /* L2: entries 384..511 = 128 × 2MB block descriptors covering
     * KBUF_VA_BASE to KBUF_VA_BASE + 256MB.
     * IPA = kbuf_gpa + (entry - 384) * 2MB.
     * Start with RW permissions — W^X toggle via handle_inst_abort
     * and handle_data_abort will adjust as rosetta needs. */
    uint64_t *l2 = pt_at(g, l2_gpa);
    for (int i = 384; i < 512; i++) {
        uint64_t ipa = kbuf_gpa + (uint64_t)(i - 384) * BLOCK_2MB;
        l2[i] = ipa | PT_AF | PT_SH_ISH | PT_ATTR1 | PT_AP_RW_EL0
               | PT_UXN | PT_PXN | PT_BLOCK;
    }

    /* Store TTBR1 value (IPA of L0 page, with ASID=0) */
    g->ttbr1 = g->ipa_base + l0_gpa;

    if (g->verbose)
        fprintf(stderr, "hl: kbuf initialized: VA 0x%llx-0x%llx → "
                "GPA 0x%llx (TTBR1=0x%llx)\n",
                (unsigned long long)KBUF_VA_BASE,
                (unsigned long long)(KBUF_VA_BASE + KBUF_SIZE - 1),
                (unsigned long long)kbuf_gpa,
                (unsigned long long)g->ttbr1);
    return 0;
}

int guest_add_va_alias(guest_t *g, uint64_t va_start,
                       uint64_t gpa_start, uint64_t size) {
    if (g->naliases >= GUEST_MAX_ALIASES) {
        fprintf(stderr, "guest: too many VA aliases (max %d)\n",
                GUEST_MAX_ALIASES);
        return -1;
    }

    guest_va_alias_t *a = &g->va_aliases[g->naliases++];
    a->va_start = va_start;
    a->gpa_start = gpa_start;
    a->size = size;
    return 0;
}

/* ---------- Overflow segment management ---------- */

/* Resolve a GPA to a host pointer within overflow segments.
 * Returns NULL if the GPA is not in any overflow segment. */
static void *guest_overflow_resolve(const guest_t *g, uint64_t gpa) {
    for (int i = 0; i < g->noverflow; i++) {
        uint64_t off = gpa - g->overflow[i].ipa_start;
        if (off < g->overflow[i].size)
            return (uint8_t *)g->overflow[i].host_base + off;
    }
    return NULL;
}

uint64_t guest_overflow_alloc(guest_t *g) {
    /* Try existing overflow segments first */
    for (int i = 0; i < g->noverflow; i++) {
        if (g->overflow[i].next + BLOCK_2MB <= g->overflow[i].size) {
            uint64_t ipa = g->overflow[i].ipa_start + g->overflow[i].next;
            g->overflow[i].next += BLOCK_2MB;
            if (g->verbose)
                fprintf(stderr, "hl: overflow alloc: 0x%llx from segment %d\n",
                        (unsigned long long)ipa, i);
            return ipa;
        }
    }

    /* Create a new overflow segment */
    if (g->noverflow >= GUEST_MAX_OVERFLOW) {
        fprintf(stderr, "hl: overflow: all %d segments exhausted\n",
                GUEST_MAX_OVERFLOW);
        return UINT64_MAX;
    }

    uint64_t seg_size = GUEST_OVERFLOW_SIZE;
    uint64_t seg_ipa  = g->overflow_ipa_next;

    /* Allocate host buffer (demand-paged, costs nothing until touched) */
    void *host = mmap(NULL, seg_size, PROT_READ | PROT_WRITE,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
    if (host == MAP_FAILED) {
        perror("hl: overflow mmap");
        return UINT64_MAX;
    }

    /* Map into the HVF VM at the overflow IPA */
    hv_return_t ret = hv_vm_map(host, seg_ipa, seg_size,
                                HV_MEMORY_READ | HV_MEMORY_WRITE |
                                HV_MEMORY_EXEC);
    if (ret != HV_SUCCESS) {
        fprintf(stderr, "hl: overflow hv_vm_map failed at IPA 0x%llx: %d\n",
                (unsigned long long)seg_ipa, (int)ret);
        munmap(host, seg_size);
        return UINT64_MAX;
    }

    int idx = g->noverflow++;
    g->overflow[idx].host_base = host;
    g->overflow[idx].ipa_start = seg_ipa;
    g->overflow[idx].size      = seg_size;
    g->overflow[idx].next      = BLOCK_2MB;  /* First block allocated */

    g->overflow_ipa_next = seg_ipa + seg_size;

    fprintf(stderr, "hl: overflow segment %d: IPA 0x%llx–0x%llx (%lluMB)\n",
            idx, (unsigned long long)seg_ipa,
            (unsigned long long)(seg_ipa + seg_size),
            (unsigned long long)(seg_size >> 20));

    return seg_ipa;  /* Return the first 2MB block's IPA */
}

int guest_host_to_gpa(const guest_t *g, const void *ptr, uint64_t *out_gpa) {
    /* Primary buffer */
    if ((const uint8_t *)ptr >= (const uint8_t *)g->host_base &&
        (const uint8_t *)ptr < (const uint8_t *)g->host_base + g->guest_size) {
        *out_gpa = (uint64_t)((const uint8_t *)ptr -
                              (const uint8_t *)g->host_base);
        return 0;
    }
    /* Overflow segments */
    for (int i = 0; i < g->noverflow; i++) {
        if ((const uint8_t *)ptr >= (const uint8_t *)g->overflow[i].host_base &&
            (const uint8_t *)ptr < (const uint8_t *)g->overflow[i].host_base +
                                    g->overflow[i].size) {
            *out_gpa = g->overflow[i].ipa_start +
                       (uint64_t)((const uint8_t *)ptr -
                                  (const uint8_t *)g->overflow[i].host_base);
            return 0;
        }
    }
    return -1;
}

void guest_destroy(guest_t *g) {
    /* Destroy all worker vCPUs (thread table) before tearing down the VM.
     * This prevents hv_vm_destroy from racing with active vCPUs that may
     * still be running if thread join timed out during exit_group. */
    thread_destroy_all_vcpus();
    if (g->vcpu) {
        hv_vcpu_destroy(g->vcpu);
        g->vcpu = 0;
    }
    /* Unmap overflow segments before VM teardown (hv_vm_unmap requires
     * the VM to still exist). Host munmap is safe regardless. */
    for (int i = 0; i < g->noverflow; i++) {
        hv_vm_unmap(g->overflow[i].ipa_start, g->overflow[i].size);
        munmap(g->overflow[i].host_base, g->overflow[i].size);
    }
    g->noverflow = 0;
    hv_vm_destroy();
    if (g->host_base) {
        munmap(g->host_base, g->guest_size);
        g->host_base = NULL;
    }
    /* Close the shm fd if we own one (parent with shm backing) */
    if (g->shm_fd >= 0) {
        close(g->shm_fd);
        g->shm_fd = -1;
    }
    /* kbuf lives within host_base — no separate free needed */
    g->kbuf_base = NULL;
    g->kbuf_gpa = 0;
    /* VA aliases reference host_base — no separate free needed */
    g->naliases = 0;
}

/* Walk guest page tables to translate VA → GPA (offset in host_base).
 * Returns the byte-level GPA or UINT64_MAX if unmapped.
 * Handles L2 block descriptors (2MB) and L3 page descriptors (4KB). */
static uint64_t guest_walk_pt(const guest_t *g, uint64_t va) {
    uint64_t base = g->ipa_base;

    /* L0 table (each entry covers 512GB) */
    const uint64_t *l0 = pt_at(g, g->ttbr0 - base);
    unsigned l0_idx = (unsigned)(va / (512ULL * BLOCK_1GB));
    if (l0_idx >= 512 || !(l0[l0_idx] & PT_VALID))
        return UINT64_MAX;

    /* L1 table (each entry covers 1GB) */
    uint64_t l1_ipa = l0[l0_idx] & 0xFFFFFFFFF000ULL;
    const uint64_t *l1 = pt_at(g, l1_ipa - base);
    unsigned l1_idx = (unsigned)((va / BLOCK_1GB) % 512);
    if (!(l1[l1_idx] & PT_VALID))
        return UINT64_MAX;

    /* L2 table (each entry covers 2MB) */
    uint64_t l2_ipa = l1[l1_idx] & 0xFFFFFFFFF000ULL;
    const uint64_t *l2 = pt_at(g, l2_ipa - base);
    unsigned l2_idx = (unsigned)((va / BLOCK_2MB) % 512);
    if (!(l2[l2_idx] & PT_VALID))
        return UINT64_MAX;

    /* Check if L2 entry is a table (→ L3) or block descriptor.
     * Block: bits[1:0]=01 (PT_VALID only). Table: bits[1:0]=11. */
    if (l2[l2_idx] & PT_TABLE) {
        /* L3 page table (each entry covers 4KB) */
        uint64_t l3_ipa = l2[l2_idx] & 0xFFFFFFFFF000ULL;
        const uint64_t *l3 = pt_at(g, l3_ipa - base);
        unsigned l3_idx = (unsigned)((va / PAGE_SIZE) % 512);
        if (!(l3[l3_idx] & PT_VALID))
            return UINT64_MAX;
        uint64_t page_ipa = l3[l3_idx] & 0xFFFFFFFFF000ULL;
        return (page_ipa - base) + (va & (PAGE_SIZE - 1));
    }

    /* Block descriptor: output IPA in bits[47:21] */
    uint64_t block_ipa = l2[l2_idx] & 0xFFFFFFE00000ULL;
    return (block_ipa - base) + (va & (BLOCK_2MB - 1));
}

/* Resolve a guest virtual address to a host pointer and available size.
 * Checks the primary mapping (host_base at ipa_base) first, then walks
 * the guest page tables for non-identity-mapped addresses (e.g., rosetta
 * at 128TB, or high-VA mmap regions used by rosetta's JIT).
 * Returns NULL if the address is not in any mapping. If avail is non-NULL,
 * stores the number of bytes available from gva to the end of its page/block. */
static void *gva_resolve(const guest_t *g, uint64_t gva, uint64_t *avail) {
    /* Fast path: primary region (identity-mapped at ipa_base) */
    if (gva >= g->ipa_base && gva < g->ipa_base + g->guest_size) {
        uint64_t off = gva - g->ipa_base;
        if (avail) *avail = g->guest_size - off;
        return (uint8_t *)g->host_base + off;
    }
    /* Legacy: allow raw offsets for internal use */
    if (gva < g->guest_size) {
        if (avail) *avail = g->guest_size - gva;
        return (uint8_t *)g->host_base + gva;
    }
    /* Kernel VA space (TTBR1): top 256MB mapped to kbuf_base.
     * Rosetta's kernel-space mmaps (MAP_FIXED at 0xFFFF....) land here. */
    if (g->kbuf_base && gva >= KBUF_VA_BASE) {
        uint64_t koff = gva - KBUF_VA_BASE;
        if (koff < KBUF_SIZE) {
            if (avail) *avail = KBUF_SIZE - koff;
            return (uint8_t *)g->kbuf_base + koff;
        }
    }
    /* Walk page tables for high VAs (rosetta binary, JIT regions, etc.).
     * This correctly handles non-identity mappings where multiple mmap
     * calls share 2MB page table blocks — each VA resolves to whatever
     * GPA the page table actually maps it to. */
    uint64_t gpa = guest_walk_pt(g, gva);
    if (gpa != UINT64_MAX) {
        /* Within a 2MB block, GPAs are always contiguous (guest_split_block
         * and guest_update_perms preserve contiguity). Report remaining
         * bytes to end of the 2MB block. */
        uint64_t block_remain = BLOCK_2MB - (gva & (BLOCK_2MB - 1));
        if (gpa < g->guest_size) {
            if (avail) *avail = block_remain;
            return (uint8_t *)g->host_base + gpa;
        }
        /* Check overflow segments (GPA beyond primary buffer) */
        void *optr = guest_overflow_resolve(g, gpa);
        if (optr) {
            if (avail) *avail = block_remain;
            return optr;
        }
    }
    return NULL;
}

void *guest_ptr(const guest_t *g, uint64_t gva) {
    return gva_resolve(g, gva, NULL);
}

void *guest_ptr_avail(const guest_t *g, uint64_t gva, uint64_t *avail) {
    return gva_resolve(g, gva, avail);
}

int guest_read(const guest_t *g, uint64_t gva, void *dst, size_t len) {
    /* Loop across 2MB block boundaries. Each gva_resolve() call returns
     * the host pointer and available bytes to the end of the current
     * 2MB block. Multi-block reads (e.g., large structs spanning a
     * block boundary) are handled by iterating. */
    size_t copied = 0;
    while (copied < len) {
        uint64_t avail;
        void *ptr = gva_resolve(g, gva + copied, &avail);
        if (!ptr) return -1;
        size_t chunk = len - copied;
        if (chunk > avail) chunk = avail;
        memcpy((uint8_t *)dst + copied, ptr, chunk);
        copied += chunk;
    }
    return 0;
}

int guest_write(guest_t *g, uint64_t gva, const void *src, size_t len) {
    /* Loop across 2MB block boundaries (see guest_read comment). */
    size_t copied = 0;
    while (copied < len) {
        uint64_t avail;
        void *ptr = gva_resolve(g, gva + copied, &avail);
        if (!ptr) return -1;
        size_t chunk = len - copied;
        if (chunk > avail) chunk = avail;
        memcpy(ptr, (const uint8_t *)src + copied, chunk);
        copied += chunk;
    }
    return 0;
}

int guest_read_str(const guest_t *g, uint64_t gva, char *dst, size_t max) {
    if (max == 0) return -1; /* Prevent underflow in max - 1 */
    size_t copied = 0;
    size_t limit = max - 1;

    /* Loop across 2MB block boundaries, same pattern as guest_read().
     * Each iteration resolves the current GVA to a contiguous host
     * region and scans for NUL within the available bytes. */
    while (copied < limit) {
        uint64_t avail;
        void *ptr = gva_resolve(g, gva + copied, &avail);
        if (!ptr) break;

        size_t remain = limit - copied;
        size_t chunk = avail < remain ? avail : remain;
        const char *src = (const char *)ptr;

        const void *nul = memchr(src, '\0', chunk);
        if (nul) {
            size_t len = (const char *)nul - src;
            memcpy(dst + copied, src, len + 1); /* Include NUL */
            return (int)(copied + len);
        }
        memcpy(dst + copied, src, chunk);
        copied += chunk;
    }

    /* Unterminated within bounds (or resolve failed) */
    dst[copied] = '\0';
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
     * Skip PROT_NONE regions — they were never touched.
     * Skip regions with GPAs beyond the primary buffer — these are
     * high-VA regions (e.g., rosetta at 128TB, kbuf user VA) whose
     * backing data either lives at a different GPA resolved through
     * VA aliases, or in extra mappings outside host_base. */
    for (int i = 0; i < g->nregions; i++) {
        guest_region_t *r = &g->regions[i];
        if (r->prot != 0 /* PROT_NONE */ && r->end > r->start &&
            r->end <= g->guest_size) {
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
    pt_pool_warned = 0;  /* Reset 80% warning for new binary */
    g->pt_pool_next = PT_POOL_BASE;
    g->brk_base = BRK_BASE_DEFAULT;
    g->brk_current = BRK_BASE_DEFAULT;
    g->mmap_next = MMAP_BASE;
    g->mmap_end = MMAP_INITIAL_END;
    g->mmap_rx_next = MMAP_RX_BASE;
    g->mmap_rx_end = MMAP_RX_INITIAL_END;
    g->ttbr0 = 0;
    g->need_tlbi = 0;
    /* Note: TTBR1 page tables are destroyed by the PT pool zeroing above.
     * Caller must call guest_init_kbuf() to rebuild them if kbuf is needed
     * (see TTBR1 Page Table Fix in CLAUDE.md). */

    /* Release overflow segments (rosetta high-VA allocations).
     * The page tables that reference them are being rebuilt, so the
     * GPA space is no longer needed. New overflow segments will be
     * allocated as needed by the next binary. */
    for (int i = 0; i < g->noverflow; i++) {
        hv_vm_unmap(g->overflow[i].ipa_start, g->overflow[i].size);
        munmap(g->overflow[i].host_base, g->overflow[i].size);
    }
    g->noverflow = 0;
    g->overflow_ipa_next = g->guest_size;

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

    /* Stack (dynamic position, stored in guest_t) */
    if (n < max) {
        out[n].offset = g->stack_base;
        out[n].size = g->stack_top - g->stack_base;
        n++;
    }

    /* mmap RW region (up to high-water mark). With the gap-finding allocator,
     * mmap_next is a high-water mark — freed regions within this range may
     * contain PROT_NONE pages (zero-fill, no cost to copy). This is
     * conservative but correct for fork state transfer. */
    if (n < max && g->mmap_next > MMAP_BASE) {
        out[n].offset = MMAP_BASE;
        out[n].size = g->mmap_next - MMAP_BASE;
        n++;
    }

    /* mmap RX region (code mappings from dynamic linker, rosetta JIT, etc.) */
    if (n < max && g->mmap_rx_next > MMAP_RX_BASE) {
        out[n].offset = MMAP_RX_BASE;
        out[n].size = g->mmap_rx_next - MMAP_RX_BASE;
        n++;
    }

    /* Rosetta segments in primary buffer (loaded near interp_base) */
    if (n < max && g->rosetta_guest_base != 0 && g->rosetta_size != 0) {
        out[n].offset = g->rosetta_guest_base;
        out[n].size = g->rosetta_size;
        n++;
    }

    /* Kernel VA buffer (256MB, used by rosetta for JIT code cache etc.) */
    if (n < max && g->kbuf_gpa != 0) {
        out[n].offset = g->kbuf_gpa;
        out[n].size = KBUF_SIZE;
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
        && a->display_va == 0 && b->display_va == 0
        && a->display_end == 0 && b->display_end == 0
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
    if (g->nregions >= GUEST_MAX_REGIONS) {
        fprintf(stderr, "guest: region table full (%d/%d), "
                "cannot track [0x%llx-0x%llx) %s\n",
                g->nregions, GUEST_MAX_REGIONS,
                (unsigned long long)start, (unsigned long long)end,
                name ? name : "");
        return -1;
    }

    /* Find insertion point (keep sorted by start address) */
    int i = g->nregions;
    while (i > 0 && g->regions[i - 1].start > start) {
        g->regions[i] = g->regions[i - 1];
        i--;
    }

    guest_region_t *r = &g->regions[i];
    r->start       = start;
    r->end         = end;
    r->prot        = prot;
    r->flags       = flags;
    r->offset      = offset;
    r->display_va  = 0;
    r->display_end = 0;
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

int guest_preannounce(guest_t *g, uint64_t start, uint64_t end,
                      int prot, int flags, uint64_t offset,
                      const char *name) {
    if (g->npreannounced >= GUEST_MAX_PREANNOUNCED) return -1;

    /* Insert sorted by start address */
    int i = g->npreannounced;
    while (i > 0 && g->preannounced[i - 1].start > start) {
        g->preannounced[i] = g->preannounced[i - 1];
        i--;
    }

    guest_region_t *r = &g->preannounced[i];
    r->start       = start;
    r->end         = end;
    r->prot        = prot;
    r->flags       = flags;
    r->offset      = offset;
    r->display_va  = 0;
    r->display_end = 0;
    if (name) {
        strncpy(r->name, name, sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
    } else {
        r->name[0] = '\0';
    }
    g->npreannounced++;
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
                /* Can't split — just trim to [r->start, start) and lose the tail.
                 * The tail [end, r->end) becomes untracked in /proc/self/maps
                 * but remains mapped in page tables. */
                fprintf(stderr, "guest: region table full, "
                        "munmap split drops tail [0x%llx-0x%llx)\n",
                        (unsigned long long)end, (unsigned long long)r->end);
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
            if (g->nregions >= GUEST_MAX_REGIONS) {
                fprintf(stderr, "guest: region table full, "
                        "mprotect split skipped at 0x%llx\n",
                        (unsigned long long)start);
                continue;
            }
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
                /* Can't split — apply prot to the whole region.
                 * The tail [end, r->end) gets new prot too. */
                fprintf(stderr, "guest: region table full, "
                        "mprotect split skipped at 0x%llx "
                        "(region [0x%llx-0x%llx) gets prot %d entirely)\n",
                        (unsigned long long)end,
                        (unsigned long long)r->start,
                        (unsigned long long)r->end, prot);
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
    g->npreannounced = 0;
}

/* ---------- Page table builder ---------- */

/* Build block descriptor for a 2MB block at the given GPA with perms. */
static uint64_t make_block_desc(uint64_t gpa, int perms) {
    uint64_t desc = (gpa & 0xFFFFFFE00000ULL) /* PA bits */
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
     *
     * For identity-mapped regions (va_base==0): VA == GPA, so L0/L1/L2
     * indices and the block descriptor output address are both derived
     * from gpa_start + ipa_base.
     *
     * For non-identity regions (va_base!=0): L0/L1/L2 indices come from
     * va_base (the virtual address), but block descriptors output the
     * GPA (physical address within the primary buffer). This is used
     * for rosetta: its VA is at 128TB, but its data lives in the primary
     * buffer at a low GPA. The MMU translates VA→IPA; HVF translates
     * IPA→host. Both translations are independent. */
    for (int r = 0; r < n; r++) {
        uint64_t gpa_start = ALIGN_2MB_DOWN(regions[r].gpa_start);
        uint64_t gpa_end   = ALIGN_2MB_UP(regions[r].gpa_end);
        int perms = regions[r].perms;

        /* Determine VA range for page table indexing */
        uint64_t va_start, va_end;
        if (regions[r].va_base != 0) {
            va_start = ALIGN_2MB_DOWN(regions[r].va_base);
            va_end = va_start + (gpa_end - gpa_start);
        } else {
            va_start = gpa_start;
            va_end = gpa_end;
        }

        uint64_t gpa_cursor = gpa_start;
        for (uint64_t va = va_start; va < va_end; va += BLOCK_2MB, gpa_cursor += BLOCK_2MB) {
            /* VA determines page table indices; GPA is the output IPA */
            uint64_t lookup_addr = base + va;    /* For L0/L1/L2 indexing */
            uint64_t output_ipa  = base + gpa_cursor; /* Block descriptor output */

            /* L0 index: which 512GB slot this VA falls in */
            unsigned l0_idx = (unsigned)(lookup_addr / (512ULL * BLOCK_1GB));
            if (l0_idx >= 512) {
                fprintf(stderr, "guest: VA 0x%llx out of L0 range\n",
                        (unsigned long long)lookup_addr);
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

            /* L1 index within the 512GB L0 entry (from VA) */
            unsigned l1_idx = (unsigned)((lookup_addr % (512ULL * BLOCK_1GB)) / BLOCK_1GB);
            if (l1_idx >= 512) {
                fprintf(stderr, "guest: VA 0x%llx out of L1 range\n",
                        (unsigned long long)lookup_addr);
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

            /* L2 index: which 2MB block within the 1GB region (from VA) */
            unsigned l2_idx = (unsigned)((lookup_addr % BLOCK_1GB) / BLOCK_2MB);

            /* If block already mapped, merge permissions (most permissive).
             * Use a local variable for the merged perms — do NOT modify
             * the outer `perms` variable, which would leak accumulated
             * permissions to subsequent 2MB blocks in the same region. */
            int block_perms = perms;
            if (l2[l2_idx] & PT_BLOCK) {
                int old_perms = 0;
                if (!(l2[l2_idx] & PT_UXN)) old_perms |= MEM_PERM_X;
                if ((l2[l2_idx] & (3ULL << 6)) == PT_AP_RW_EL0)
                    old_perms |= MEM_PERM_W;
                old_perms |= MEM_PERM_R;
                block_perms |= old_perms;
            }

            /* Block descriptor: output IPA (where data physically lives) */
            l2[l2_idx] = make_block_desc(output_ipa, block_perms);
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
        if (l0_idx >= 512) {
            fprintf(stderr, "guest: IPA 0x%llx out of L0 range in extend\n",
                    (unsigned long long)ipa);
            return -1;
        }

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

/* ---------- Non-identity page table mapping ---------- */

int guest_map_va_range(guest_t *g, uint64_t va_start, uint64_t va_end,
                       uint64_t gpa_start, int perms) {
    uint64_t base = g->ipa_base;

    /* Navigate to L0 table */
    uint64_t l0_gpa_off = g->ttbr0 - base;
    uint64_t *l0 = pt_at(g, l0_gpa_off);

    uint64_t va_aligned  = ALIGN_2MB_DOWN(va_start);
    uint64_t va_end_aligned = ALIGN_2MB_UP(va_end);
    uint64_t gpa_aligned = ALIGN_2MB_DOWN(gpa_start);

    uint64_t gpa_cursor = gpa_aligned;
    for (uint64_t va = va_aligned; va < va_end_aligned;
         va += BLOCK_2MB, gpa_cursor += BLOCK_2MB) {

        /* L0 index: which 512GB slot this VA falls in */
        unsigned l0_idx = (unsigned)(va / (512ULL * BLOCK_1GB));
        if (l0_idx >= 512) {
            fprintf(stderr, "guest: VA 0x%llx out of L0 range in map_va_range\n",
                    (unsigned long long)va);
            return -1;
        }

        /* Allocate L1 table on first access to each L0 slot */
        if (!(l0[l0_idx] & PT_VALID)) {
            uint64_t l1_gpa = pt_alloc_page(g);
            if (!l1_gpa) return -1;
            l0[l0_idx] = (base + l1_gpa) | PT_VALID | PT_TABLE;
        }

        uint64_t l1_ipa = l0[l0_idx] & 0xFFFFFFFFF000ULL;
        uint64_t *l1 = pt_at(g, l1_ipa - base);
        unsigned l1_idx = (unsigned)((va % (512ULL * BLOCK_1GB)) / BLOCK_1GB);

        /* Allocate L2 table on first access */
        if (!(l1[l1_idx] & PT_VALID)) {
            uint64_t l2_gpa = pt_alloc_page(g);
            if (!l2_gpa) return -1;
            l1[l1_idx] = (base + l2_gpa) | PT_VALID | PT_TABLE;
        }

        uint64_t l2_ipa = l1[l1_idx] & 0xFFFFFFFFF000ULL;
        uint64_t *l2 = pt_at(g, l2_ipa - base);
        unsigned l2_idx = (unsigned)((va % BLOCK_1GB) / BLOCK_2MB);

        /* Block descriptor: output IPA points to GPA in primary buffer */
        uint64_t output_ipa = base + gpa_cursor;
        if (!(l2[l2_idx] & PT_BLOCK)) {
            l2[l2_idx] = make_block_desc(output_ipa, perms);
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
    if (l0_idx >= 512 || !(l0[l0_idx] & PT_VALID)) return NULL;

    uint64_t l1_ipa = l0[l0_idx] & 0xFFFFFFFFF000ULL;
    uint64_t *l1 = pt_at(g, l1_ipa - base);

    unsigned l1_idx = (unsigned)((ipa % (512ULL * BLOCK_1GB)) / BLOCK_1GB);
    if (l1_idx >= 512 || !(l1[l1_idx] & PT_VALID)) return NULL;

    uint64_t l2_ipa = l1[l1_idx] & 0xFFFFFFFFF000ULL;
    uint64_t *l2 = pt_at(g, l2_ipa - base);

    unsigned l2_idx = (unsigned)((ipa % BLOCK_1GB) / BLOCK_2MB);
    return &l2[l2_idx];
}

/* Split a 2MB L2 block descriptor into 512 × 4KB L3 page descriptors.
 * Shared between TTBR0 (guest_split_block) and TTBR1 (guest_kbuf_split_block).
 * The caller provides the L2 entry via their respective find_l2_entry path.
 * Extracts the output IPA from the existing descriptor — correct for both
 * identity-mapped (VA=GPA) and non-identity (high VA→low GPA) regions. */
static int split_l2_block(guest_t *g, uint64_t *l2_entry) {
    if (!l2_entry) return -1;

    /* Already a table descriptor (previously split) — nothing to do */
    if ((*l2_entry & 3) == 3) return 0;

    /* Must be a valid block descriptor: bit[0]=1, bit[1]=0 */
    if (!(*l2_entry & PT_BLOCK)) return -1;

    int old_perms = desc_to_perms(*l2_entry);

    uint64_t l3_gpa = pt_alloc_page(g);
    if (!l3_gpa) return -1;
    uint64_t *l3 = pt_at(g, l3_gpa);

    /* Fill 512 L3 entries with 4KB page descriptors inheriting the
     * block's permissions.  Extract the output IPA from bits [47:21]
     * of the existing descriptor (not from the caller's address). */
    uint64_t block_ipa = *l2_entry & 0xFFFFFFE00000ULL;
    for (int i = 0; i < 512; i++)
        l3[i] = make_page_desc(block_ipa + (uint64_t)i * PAGE_SIZE, old_perms);

    *l2_entry = (g->ipa_base + l3_gpa) | PT_VALID | PT_TABLE;
    g->need_tlbi = 1;
    return 0;
}

int guest_split_block(guest_t *g, uint64_t block_gpa) {
    uint64_t block_start = ALIGN_2MB_DOWN(block_gpa);
    uint64_t *l2_entry = find_l2_entry(g, block_start);
    return split_l2_block(g, l2_entry);
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

/* ---------- Kernel VA (TTBR1) page table helpers ---------- */

/* Find the L2 entry for a kernel VA address (TTBR1 region).
 * Kernel VA L2 table is at fixed location: L0[511]→L1[511]→L2.
 * Returns pointer to the L2 entry, or NULL if kbuf not initialized. */
static uint64_t *kbuf_find_l2_entry(guest_t *g, uint64_t va) {
    if (!g->kbuf_base || va < KBUF_VA_BASE) return NULL;

    /* TTBR1 L0 → L1 → L2 chain was built by guest_init_kbuf.
     * We know the structure: L0[511]→L1, L1[511]→L2.
     * Extract L2 table IPA from the chain. */
    uint64_t base = g->ipa_base;
    const uint64_t *l0 = pt_at(g, g->ttbr1 - base);
    if (!(l0[511] & PT_VALID)) return NULL;

    uint64_t l1_ipa = l0[511] & 0xFFFFFFFFF000ULL;
    const uint64_t *l1 = pt_at(g, l1_ipa - base);
    if (!(l1[511] & PT_VALID)) return NULL;

    uint64_t l2_ipa = l1[511] & 0xFFFFFFFFF000ULL;
    uint64_t *l2 = (uint64_t *)pt_at(g, l2_ipa - base);

    /* L2 index from the VA's bits [29:21] */
    unsigned l2_idx = (unsigned)((va >> 21) & 0x1FF);
    return &l2[l2_idx];
}

int guest_kbuf_split_block(guest_t *g, uint64_t block_va) {
    uint64_t *l2_entry = kbuf_find_l2_entry(g, block_va);
    return split_l2_block(g, l2_entry);
}

int guest_kbuf_update_perms(guest_t *g, uint64_t start, uint64_t end, int perms) {
    start = start & ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start; addr < end; ) {
        uint64_t *l2_entry = kbuf_find_l2_entry(g, addr);
        if (!l2_entry || !(*l2_entry & 1)) {
            addr = ALIGN_2MB_UP(addr + 1);
            continue;
        }
        uint64_t block_start = addr & ~(BLOCK_2MB - 1);
        uint64_t block_end = block_start + BLOCK_2MB;

        if ((*l2_entry & 3) == 1) {
            /* 2MB block: whole-block update or split */
            int old_perms = desc_to_perms(*l2_entry);
            if (start <= block_start && end >= block_end) {
                if (old_perms != perms) {
                    uint64_t ipa = *l2_entry & 0xFFFFFFE00000ULL;
                    *l2_entry = make_block_desc(ipa, perms);
                    g->need_tlbi = 1;
                }
                addr = block_end;
                continue;
            }
            if (old_perms != perms) {
                if (guest_kbuf_split_block(g, block_start) < 0) return -1;
            } else {
                addr = block_end;
                continue;
            }
        }

        /* L3: update individual pages */
        uint64_t l3_ipa = *l2_entry & 0xFFFFFFFFF000ULL;
        uint64_t *l3 = (uint64_t *)pt_at(g, l3_ipa - g->ipa_base);
        uint64_t page_start = (addr > block_start) ? addr : block_start;
        uint64_t page_end = (end < block_end) ? end : block_end;
        for (uint64_t pa = page_start; pa < page_end; pa += PAGE_SIZE) {
            unsigned l3_idx = (unsigned)((pa % BLOCK_2MB) / PAGE_SIZE);
            if (!(l3[l3_idx] & PT_VALID)) continue; /* skip invalidated */
            uint64_t page_ipa = l3[l3_idx] & 0xFFFFFFFFF000ULL;
            l3[l3_idx] = make_page_desc(page_ipa, perms);
        }
        g->need_tlbi = 1;
        addr = page_end;
    }
    return 0;
}

int guest_kbuf_invalidate_ptes(guest_t *g, uint64_t start, uint64_t end) {
    start = start & ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start; addr < end; ) {
        uint64_t *l2_entry = kbuf_find_l2_entry(g, addr);
        if (!l2_entry || !(*l2_entry & 1)) {
            addr = ALIGN_2MB_UP(addr + 1);
            continue;
        }
        uint64_t block_start = addr & ~(BLOCK_2MB - 1);
        uint64_t block_end = block_start + BLOCK_2MB;

        if ((*l2_entry & 3) == 1) {
            /* 2MB block: whole-block invalidation or split */
            if (start <= block_start && end >= block_end) {
                *l2_entry = 0;
                g->need_tlbi = 1;
                addr = block_end;
                continue;
            }
            /* Partial: split into 4KB L3 pages first */
            if (guest_kbuf_split_block(g, block_start) < 0) return -1;
        }

        /* L3 table: invalidate individual 4KB page descriptors */
        uint64_t l3_ipa = *l2_entry & 0xFFFFFFFFF000ULL;
        uint64_t *l3 = (uint64_t *)pt_at(g, l3_ipa - g->ipa_base);
        uint64_t page_start = (addr > block_start) ? addr : block_start;
        uint64_t page_end = (end < block_end) ? end : block_end;
        for (uint64_t pa = page_start; pa < page_end; pa += PAGE_SIZE) {
            unsigned l3_idx = (unsigned)((pa % BLOCK_2MB) / PAGE_SIZE);
            l3[l3_idx] = 0;  /* Invalid descriptor → translation fault */
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
             * actually differ, just rewrite the block descriptor. Extract
             * the output IPA from the existing descriptor — correct for
             * both identity and non-identity mapped regions. */
            if (start <= block_start && end >= block_end) {
                if (old_perms != perms) {
                    uint64_t ipa = *l2_entry & 0xFFFFFFE00000ULL;
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
            /* Extract the existing output IPA from the L3 entry. For
             * non-identity mapped regions, pa is a VA not a GPA, so we
             * must use the IPA already stored in the descriptor (set by
             * guest_split_block).
             *
             * For invalidated entries (set to 0 by guest_invalidate_ptes),
             * the stored IPA is 0 — wrong. Fall back to computing the
             * identity-mapped IPA (base + pa). This is correct for TTBR0
             * user-space regions where VA == IPA == GPA. Non-identity
             * mapped regions (rosetta VA aliases) should never have
             * invalidated entries that get re-activated here. */
            uint64_t page_ipa;
            if (l3[l3_idx] & PT_VALID)
                page_ipa = l3[l3_idx] & 0xFFFFFFFFF000ULL;
            else
                page_ipa = base + (pa & ~(PAGE_SIZE - 1));
            l3[l3_idx] = make_page_desc(page_ipa, perms);
        }

        g->need_tlbi = 1;
        addr = page_end;
    }

    return 0;
}

