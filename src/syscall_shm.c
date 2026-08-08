/*
 * syscall_shm.c — SysV SHM via host shmget/shmat + HVF mapping into guest
 *
 * Copyright (c) 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Why: Linux X clients (GTK1/XMMS) use MIT-SHM when the display is local.
 * Guest shmget without host-backed segments makes XQuartz shmat() fail, and
 * some clients never successfully fall back to PutImage for skins.
 * Returning real host shmids lets XQuartz attach the same pages the guest
 * paints into.
 *
 * Mapping strategy:
 *  1. shmget/shmat on the host so the shmid is real for XQuartz.
 *  2. Steal a 2MB-aligned IPA slice inside the primary Stage-2 map, unmap it,
 *     and remmap the SysV pages there (page-rounded size — never larger than
 *     the host SHM mapping, or hv_vm_map returns HV_ERROR).
 *  3. Install non-identity Stage-1 PTEs: guest_va → stolen IPA.
 *  4. guest_ptr() resolves guest_va via hl_shm_resolve() before the identity
 *     fast path so host-side copies hit the SysV pages.
 */
#include "syscall_shm.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "guest.h"
#include "trace.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <unistd.h>
#include <mach/vm_page_size.h>
#include <Hypervisor/Hypervisor.h>

/* Linux aarch64 shm flags / cmds (subset). */
#ifndef LINUX_IPC_CREAT
#define LINUX_IPC_CREAT  0001000
#define LINUX_IPC_EXCL   0002000
#define LINUX_IPC_NOWAIT 0004000
#define LINUX_IPC_RMID   0
#define LINUX_IPC_SET    1
#define LINUX_IPC_STAT   2
#define LINUX_IPC_INFO   3
#define LINUX_SHM_INFO   14
#define LINUX_SHM_STAT   13
#define LINUX_SHM_RDONLY 010000
#define LINUX_SHM_REMAP  040000
#define LINUX_SHM_LOCK   11
#define LINUX_SHM_UNLOCK 12
#endif

/* Linux struct shminfo / shm_info (asm-generic, 64-bit). */
typedef struct {
    uint64_t shmmax, shmmin, shmmni, shmseg, shmall;
    uint64_t unused[4];
} linux_shminfo_t;

typedef struct {
    int32_t  used_ids;
    int32_t  pad;
    uint64_t shm_tot, shm_rss, shm_swp;
    uint64_t swap_attempts, swap_successes;
} linux_shm_info_t;

#define LINUX_IPC_PRIVATE 0

#define HL_SHM_MAX 64
#define HL_SHM_2MB  (2ULL * 1024 * 1024)

/*
 * HVF Stage-2 maps require host page alignment. On Apple Silicon that is
 * typically 16KB (vm_page_size), NOT the guest Linux 4KB page size.
 * Mapping a SysV segment with a 4KB-rounded size (e.g. 0xb000) fails with
 * HV_BAD_ARGUMENT (-85377021). Always round Stage-2 sizes to vm_page_size.
 */
static size_t
hl_host_page_size(void)
{
    size_t psz = (size_t)vm_page_size;
    if (psz < 4096)
        psz = 4096;
    return psz;
}

typedef struct {
    int used;
    int host_shmid;
    size_t size;       /* logical segment size (from shmget/shmctl) */
    size_t map_size;   /* page-rounded size actually hv_vm_map'd */
    size_t ipa_span;   /* 2MB-rounded IPA reservation (for unmap/restore) */
    void *host_addr;   /* shmat in hl process */
    uint64_t guest_va; /* most recent attach address (2MB-aligned) */
    /* ALL live attach windows. A second shmat() of the same segment used to
     * overwrite guest_va, orphaning the first window: va_range_busy() then
     * reported it free, first-fit handed it to a different segment, and the
     * still-valid L2 block made guest_map_va_range() a silent no-op — so
     * the new segment's writes landed in the old segment's shared pages.
     * shmdt() of the first view also returned EINVAL. */
#define HL_SHM_MAX_ATTACH 8
    uint64_t attach_va[HL_SHM_MAX_ATTACH];
    int      nattach_va;
    uint64_t ipa;      /* HVF IPA of host_addr (2MB-aligned) */
    int nattach;
    int stage2_stolen; /* 1 if we replaced primary Stage-2 at ipa */
    int rmid_pending;  /* IPC_RMID seen while still attached (deferred free) */
} hl_shm_seg_t;

static hl_shm_seg_t segs[HL_SHM_MAX];
static uint64_t hl_shm_find_guest_va(guest_t *g, size_t ipa_span);
static pthread_mutex_t shm_lock = PTHREAD_MUTEX_INITIALIZER;
static int resolve_hooked;

static size_t
align_host_page(size_t n)
{
    size_t psz = hl_host_page_size();
    return (n + psz - 1) & ~(psz - 1);
}

static size_t
align_2mb(size_t n)
{
    return (n + (size_t)HL_SHM_2MB - 1) & ~((size_t)HL_SHM_2MB - 1);
}

static void *
hl_shm_resolve(uint64_t gva, uint64_t *avail)
{
    int i;
    pthread_mutex_lock(&shm_lock);
    for (i = 0; i < HL_SHM_MAX; i++) {
        hl_shm_seg_t *s = &segs[i];
        if (!s->used || !s->host_addr || s->nattach <= 0)
            continue;
        /* Resolve within the logical SHM size (what the client may touch).
         * Every attach window resolves — a second shmat() used to leave the
         * first window pointing at whatever primary RAM the stale PTE
         * covered. */
        for (int k = 0; k < s->nattach_va; k++) {
            uint64_t va = s->attach_va[k];
            if (!va) continue;
            if (gva >= va && gva < va + s->map_size) {
                uint64_t off = gva - va;
                if (avail)
                    *avail = s->map_size - off;
                pthread_mutex_unlock(&shm_lock);
                return (uint8_t *)s->host_addr + off;
            }
        }
    }
    pthread_mutex_unlock(&shm_lock);
    return NULL;
}

static void
hl_shm_ensure_hook(void)
{
    if (!resolve_hooked) {
        guest_set_extra_resolve(hl_shm_resolve);
        resolve_hooked = 1;
    }
}

/* Restore primary Stage-2 for a stolen IPA slice (best-effort). */
static void
hl_shm_restore_stage2(guest_t *g, uint64_t ipa, size_t ipa_span)
{
    hv_return_t hv;
    void *host;

    if (!g || !g->host_base || ipa_span == 0)
        return;
    if (ipa + ipa_span > g->guest_size)
        return;
    (void)hv_vm_unmap(ipa, ipa_span);
    host = (uint8_t *)g->host_base + ipa;
    hv = hv_vm_map(host, ipa, ipa_span,
                   HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (hv != HV_SUCCESS) {
        fprintf(stderr,
                "hl: shm restore Stage-2 ipa=0x%llx span=0x%zx → %d\n",
                (unsigned long long)ipa, ipa_span, (int)hv);
    }
}

/* Linux shmid_ds is large; we only need shm_segsz for IPC_STAT consumers. */
typedef struct {
    uint32_t __key;
    uint32_t uid, gid, cuid, cgid;
    uint16_t mode;
    uint16_t __pad1;
    uint16_t __seq;
    uint16_t __pad2;
    uint64_t __unused1, __unused2;
    uint64_t shm_segsz;
    int64_t shm_atime, shm_dtime, shm_ctime;
    int32_t shm_cpid, shm_lpid;
    uint64_t shm_nattch;
    uint64_t __unused3, __unused4;
} linux_shmid_ds_t;

static hl_shm_seg_t *
find_by_shmid(int shmid)
{
    int i;
    for (i = 0; i < HL_SHM_MAX; i++) {
        if (segs[i].used && segs[i].host_shmid == shmid)
            return &segs[i];
    }
    return NULL;
}

static hl_shm_seg_t *
find_by_guest_va(uint64_t va)
{
    int i;
    for (i = 0; i < HL_SHM_MAX; i++) {
        if (!segs[i].used || segs[i].nattach <= 0) continue;
        for (int k = 0; k < segs[i].nattach_va; k++)
            if (segs[i].attach_va[k] == va)
                return &segs[i];
    }
    return NULL;
}

/* Drop one attach window from a segment's list (order irrelevant). */
static void
shm_forget_va(hl_shm_seg_t *s, uint64_t va)
{
    for (int k = 0; k < s->nattach_va; k++) {
        if (s->attach_va[k] == va) {
            s->attach_va[k] = s->attach_va[--s->nattach_va];
            s->attach_va[s->nattach_va] = 0;
            return;
        }
    }
}

/* True when [lo, lo+span) overlaps any live segment's IPA reservation. */
static int
ipa_range_busy(uint64_t lo, size_t span)
{
    for (int i = 0; i < HL_SHM_MAX; i++) {
        if (!segs[i].used || !segs[i].stage2_stolen || !segs[i].ipa) continue;
        uint64_t a = segs[i].ipa, b = a + segs[i].ipa_span;
        if (lo < b && a < lo + span) return 1;
    }
    return 0;
}

/* True when [lo, lo+span) overlaps any live segment's guest VA window. */
static int
va_range_busy(uint64_t lo, size_t span)
{
    for (int i = 0; i < HL_SHM_MAX; i++) {
        if (!segs[i].used) continue;
        for (int k = 0; k < segs[i].nattach_va; k++) {
            uint64_t a = segs[i].attach_va[k], b = a + segs[i].ipa_span;
            if (a && lo < b && a < lo + span) return 1;
        }
    }
    return 0;
}

static hl_shm_seg_t *
alloc_slot(void)
{
    int i;
    for (i = 0; i < HL_SHM_MAX; i++) {
        if (!segs[i].used) {
            memset(&segs[i], 0, sizeof(segs[i]));
            segs[i].used = 1;
            return &segs[i];
        }
    }
    return NULL;
}

int64_t
sys_shmget(guest_t *g, int64_t key, uint64_t size, int shmflg)
{
    int host_flags = 0;
    int host_id;
    hl_shm_seg_t *s;
    key_t hkey;

    (void)g;
    if (size == 0)
        return -LINUX_EINVAL;

    if (shmflg & LINUX_IPC_CREAT)
        host_flags |= IPC_CREAT;
    if (shmflg & LINUX_IPC_EXCL)
        host_flags |= IPC_EXCL;
    host_flags |= (shmflg & 0777);

    hkey = (key == LINUX_IPC_PRIVATE) ? IPC_PRIVATE : (key_t)key;

    host_id = shmget(hkey, (size_t)size, host_flags);
    if (host_id < 0)
        return linux_errno();

    pthread_mutex_lock(&shm_lock);
    s = find_by_shmid(host_id);
    if (!s) {
        s = alloc_slot();
        if (s) {
            s->host_shmid = host_id;
            s->size = (size_t)size;
            s->map_size = align_host_page((size_t)size);
            s->ipa_span = align_2mb(s->map_size);
        }
    }
    pthread_mutex_unlock(&shm_lock);

    if (!s) {
        shmctl(host_id, IPC_RMID, NULL);
        return -LINUX_ENOSPC;
    }

    hl_trace(HL_TRACE_SYS, "shmget key=%lld size=%llu flags=0%o → shmid=%d",
             (long long)key, (unsigned long long)size, shmflg, host_id);
    return host_id;
}

int64_t
sys_shmat(guest_t *g, int shmid, uint64_t shmaddr, int shmflg)
{
    hl_shm_seg_t *s;
    void *host;
    int host_flg = 0;
    uint64_t guest_va, ipa;
    hv_return_t hv;
    size_t map_size, ipa_span;
    struct shmid_ds ds;

    if (shmaddr != 0 && (shmaddr & 4095))
        return -LINUX_EINVAL;

    pthread_mutex_lock(&shm_lock);
    int slot_is_new = 0;
    s = find_by_shmid(shmid);
    if (!s) {
        /* Adopt a segment we created but no longer track — but ONLY our
         * own. Adopting any shmid the guest names let it enumerate and map
         * the SysV segments of unrelated same-uid host processes straight
         * into its address space. */
        if (shmctl(shmid, IPC_STAT, &ds) != 0) {
            pthread_mutex_unlock(&shm_lock);
            return -LINUX_EINVAL;
        }
        if (ds.shm_cpid != getpid()) {
            pthread_mutex_unlock(&shm_lock);
            return -LINUX_EACCES;
        }
        s = alloc_slot();
        if (s) {
            slot_is_new = 1;
            s->host_shmid = shmid;
            s->size = ds.shm_segsz ? (size_t)ds.shm_segsz : (size_t)HL_SHM_2MB;
            s->map_size = align_host_page(s->size);
            s->ipa_span = align_2mb(s->map_size);
        }
    }
    if (!s) {
        pthread_mutex_unlock(&shm_lock);
        return -LINUX_ENOSPC;
    }
    /* Free the slot again on any error below, or 64 failed attaches would
     * exhaust the table and disable SysV SHM for the whole process. */
    #define SHMAT_FAIL(rc) do {                       \
        if (slot_is_new) memset(s, 0, sizeof(*s));    \
        pthread_mutex_unlock(&shm_lock);              \
        return (rc);                                  \
    } while (0)

    /* Refresh size from kernel (actual rounded size). */
    if (shmctl(shmid, IPC_STAT, &ds) == 0 && ds.shm_segsz > 0) {
        s->size = ds.shm_segsz;
        s->map_size = align_host_page(ds.shm_segsz);
        s->ipa_span = align_2mb(s->map_size);
    }

    if (shmflg & LINUX_SHM_RDONLY)
        host_flg |= SHM_RDONLY;

    if (!s->host_addr) {
        host = shmat(shmid, NULL, host_flg);
        if (host == (void *)(intptr_t)-1) {
            int e = errno;
            fprintf(stderr, "hl: host shmat(%d) failed: %s (flg=0%o)\n",
                    shmid, strerror(e), host_flg);
            errno = e;
            int lrc = (int)linux_errno();
            SHMAT_FAIL(lrc);
        }
        s->host_addr = host;

        /*
         * Host page alignment for HVF. macOS SysV segments are at least
         * host-page-backed; round the Stage-2 size up so hv_vm_map accepts it
         * even when the logical segsz is only 4KB-rounded (Linux convention).
         */
        map_size = s->map_size;
        if (map_size == 0)
            map_size = align_host_page(s->size ? s->size : hl_host_page_size());
        map_size = align_host_page(map_size);
        ipa_span = s->ipa_span ? s->ipa_span : align_2mb(map_size);
        if (ipa_span < map_size)
            ipa_span = align_2mb(map_size);

        /*
         * Steal a 2MB-aligned IPA slice inside the primary buffer's
         * reserved high zone [mmap_limit, guest_size). That band is not
         * used by the mmap gap allocator, so we do not stomp live guest
         * identity maps. Stage-2 for the slice is replaced with SysV pages.
         *
         * map_size is page-rounded logical size only — mapping a full 2MB
         * when the SHM is smaller walks unmapped host VA and HVF returns
         * HV_ERROR (-85377023).
         */
        {
            /*
             * Reserved band is [mmap_limit, guest_size):
             *   [mmap_limit, mmap_limit+2GB)  — guest VA for SHM (hl_shm_find_guest_va)
             *   [mmap_limit+2GB, interp_base) — IPA steal (this block)
             *   [interp_base, guest_size)     — dynamic linker
             */
            uint64_t zone_lo = g->mmap_limit + 0x80000000ULL; /* +2GB */
            uint64_t zone_hi = g->interp_base;
            if (zone_lo >= zone_hi || zone_hi - zone_lo < ipa_span) {
                /* Compact / unexpected layout: use last 256MB under guest_size. */
                zone_hi = g->guest_size;
                zone_lo = (zone_hi > 0x10000000ULL) ? zone_hi - 0x10000000ULL : 0;
            }
            zone_lo = (zone_lo + HL_SHM_2MB - 1) & ~(HL_SHM_2MB - 1);

            /* First fit over the band. This used to be a monotonic bump
             * cursor that release never rewound, so ~1024 attach/detach
             * cycles exhausted the 2GB zone even though every segment had
             * been freed — a GTK client recreating its XShmImage on each
             * resize hit that. */
            ipa = 0;
            for (uint64_t cand = zone_lo; cand + ipa_span <= zone_hi;
                 cand += HL_SHM_2MB) {
                if (!ipa_range_busy(cand, ipa_span)) { ipa = cand; break; }
            }
            if (ipa == 0) {
                fprintf(stderr,
                        "hl: shmat: no room to steal IPA "
                        "(span=0x%zx zone=[0x%llx,0x%llx))\n",
                        ipa_span,
                        (unsigned long long)zone_lo,
                        (unsigned long long)zone_hi);
                shmdt(host);
                s->host_addr = NULL;
                SHMAT_FAIL(-LINUX_ENOMEM);
            }
        }

        /* Drop primary Stage-2 for the reservation, then map SHM pages. */
        hv = hv_vm_unmap(ipa, ipa_span);
        if (hv != HV_SUCCESS) {
            fprintf(stderr,
                    "hl: shmat hv_vm_unmap ipa=0x%llx span=0x%zx → %d "
                    "(continuing)\n",
                    (unsigned long long)ipa, ipa_span, (int)hv);
        }

        hv = hv_vm_map(host, ipa, map_size,
                       HV_MEMORY_READ | HV_MEMORY_WRITE);
        if (hv != HV_SUCCESS) {
            fprintf(stderr,
                    "hl: shmat hv_vm_map host=%p ipa=0x%llx sz=0x%zx → %d\n",
                    host, (unsigned long long)ipa, map_size, (int)hv);
            /* Restore primary for the full reservation. */
            hl_shm_restore_stage2(g, ipa, ipa_span);
            shmdt(host);
            s->host_addr = NULL;
            SHMAT_FAIL(-LINUX_ENOMEM);
        }

        s->ipa = ipa;
        s->map_size = map_size;
        s->ipa_span = ipa_span;
        s->stage2_stolen = 1;
        hl_shm_ensure_hook();
    } else {
        host = s->host_addr;
        ipa = s->ipa;
        map_size = s->map_size;
        ipa_span = s->ipa_span;
    }

    /* Choose guest VA (2MB-aligned for guest_map_va_range). */
    if (shmaddr != 0) {
        if (shmaddr & (HL_SHM_2MB - 1)) {
            /* Linux allows page-aligned attach; we need 2MB for L2 blocks.
             * Reject non-2MB hints rather than corrupt neighbouring VAs. */
            fprintf(stderr,
                    "hl: shmat: shmaddr 0x%llx not 2MB-aligned\n",
                    (unsigned long long)shmaddr);
            SHMAT_FAIL(-LINUX_EINVAL);
        }
        guest_va = shmaddr;
        if (va_range_busy(guest_va, ipa_span)) {
            /* An explicit shmaddr onto an occupied window used to "succeed"
             * and then quietly alias: guest_map_va_range leaves an already
             * valid L2 block alone, so the guest kept reading the previous
             * segment. Linux returns EINVAL without SHM_REMAP; so do we. */
            SHMAT_FAIL(-LINUX_EINVAL);
        }
    } else {
        guest_va = hl_shm_find_guest_va(g, ipa_span);
        if (guest_va == 0) {
            fprintf(stderr, "hl: shmat: no guest VA\n");
            SHMAT_FAIL(-LINUX_ENOMEM);
        }
    }

    /*
     * Stage-1: guest_va → ipa (non-identity). guest_map_va_range takes
     * gpa_start as an offset from ipa_base (0), so pass the IPA itself.
     * Map the full 2MB span so the L2 block is valid; Stage-2 only backs
     * map_size bytes — clients must not touch past the segment.
     */
    if (s->nattach_va >= HL_SHM_MAX_ATTACH)
        SHMAT_FAIL(-LINUX_ENOMEM);
    int va_skipped = 0;
    if (guest_map_va_range_ex(g, guest_va, guest_va + ipa_span, ipa,
                              MEM_PERM_RW, &va_skipped) != 0) {
        fprintf(stderr, "hl: shmat map_va failed va=0x%llx ipa=0x%llx\n",
                (unsigned long long)guest_va, (unsigned long long)ipa);
        SHMAT_FAIL(-LINUX_ENOMEM);
    }
    if (va_skipped) {
        /* "First mapping wins" is right for mmap's high-VA reuse but is
         * silent corruption here: the attach would report success while the
         * guest still saw the old contents. Fail loudly instead. */
        fprintf(stderr,
                "hl: shmat: VA 0x%llx already mapped (%d blocks); refusing "
                "to alias\n", (unsigned long long)guest_va, va_skipped);
        guest_unmap_va_range(g, guest_va, guest_va + ipa_span);
        SHMAT_FAIL(-LINUX_EINVAL);
    }
    guest_region_add(g, guest_va, guest_va + map_size, MEM_PERM_RW,
                     0 /* flags */, 0 /* offset */, "[sysvshm]");

    s->guest_va = guest_va;
    s->attach_va[s->nattach_va++] = guest_va;
    s->nattach++;
    g->need_tlbi = 1;
    pthread_mutex_unlock(&shm_lock);

    hl_trace(HL_TRACE_SYS,
             "shmat shmid=%d → guest_va=0x%llx host=%p ipa=0x%llx "
             "map=0x%zx span=0x%zx",
             shmid, (unsigned long long)guest_va, host,
             (unsigned long long)ipa, map_size, ipa_span);
    return (int64_t)guest_va;
}

/* Bump allocator in reserved VA above mmap_limit (mmap gap allocator stays clear). */
static uint64_t
hl_shm_find_guest_va(guest_t *g, size_t ipa_span)
{
    uint64_t va;
    size_t span = ipa_span ? ipa_span : (size_t)HL_SHM_2MB;
    /* Guest VA in the lower half of the reserved band; IPA steals the upper
     * half (see sys_shmat steal zone). Keeps VA ≠ IPA (non-identity PTEs). */
    uint64_t va_lo = g->mmap_limit;
    uint64_t va_hi = g->mmap_limit + 0x80000000ULL; /* 2GB of reserved VA */
    if (va_hi > g->interp_base)
        va_hi = g->interp_base;
    if (va_hi <= va_lo + span) {
        /* Compact primaries: park just under mmap_limit (still rare for mmap). */
        va_hi = g->mmap_limit;
        va_lo = (va_hi > 0x20000000ULL) ? va_hi - 0x20000000ULL : MMAP_BASE;
    }
    va_lo = (va_lo + HL_SHM_2MB - 1) & ~(HL_SHM_2MB - 1);

    /* First fit over the band, so a detached window is reusable. The old
     * monotonic `next_va` was never rewound on release. */
    for (va = va_lo; va + span <= va_hi; va += HL_SHM_2MB) {
        if (!va_range_busy(va, span))
            return va;
    }
    return 0;
}

/*
 * Drop hl's own attachment and give the stolen IPA slice back to the primary
 * Stage-2 map. Caller holds shm_lock. Only safe once the guest has detached.
 */
static void
shm_release_locked(guest_t *g, hl_shm_seg_t *s)
{
    if (s->host_addr) {
        shmdt(s->host_addr);
        s->host_addr = NULL;
    }
    if (s->stage2_stolen && s->ipa) {
        hl_shm_restore_stage2(g, s->ipa, s->ipa_span);
        s->stage2_stolen = 0;
    }
    memset(s, 0, sizeof(*s));
}

int64_t
sys_shmdt(guest_t *g, uint64_t shmaddr)
{
    hl_shm_seg_t *s;

    pthread_mutex_lock(&shm_lock);
    s = find_by_guest_va(shmaddr);
    if (!s) {
        pthread_mutex_unlock(&shm_lock);
        return -LINUX_EINVAL;
    }
    if (s->nattach > 0)
        s->nattach--;
    /* Invalidate the Stage-1 mapping for THIS window. Without this the 2MB
     * window stayed valid and writable after detach and aliased primary
     * guest RAM once the IPA slice was restored — Linux would SIGSEGV.
     * Detaching one of several windows must not tear down the others. */
    guest_unmap_va_range(g, shmaddr, shmaddr + s->ipa_span);
    guest_region_remove(g, shmaddr, shmaddr + s->map_size);
    shm_forget_va(s, shmaddr);
    if (s->guest_va == shmaddr)
        s->guest_va = s->nattach_va ? s->attach_va[0] : 0;
    if (s->nattach == 0) {
        s->nattach_va = 0;
        s->guest_va = 0;
        if (s->rmid_pending) {
            /* Marked destroyed earlier; last guest detach frees it now. */
            shm_release_locked(g, s);
        }
        /* Else keep host mapping + Stage-2 for X server attach. */
    }
    pthread_mutex_unlock(&shm_lock);
    g->need_tlbi = 1;
    return 0;
}

int64_t
sys_shmctl(guest_t *g, int shmid, int cmd, uint64_t buf_gva)
{
    hl_shm_seg_t *s;
    struct shmid_ds ds;
    linux_shmid_ds_t lds;

    if (cmd == LINUX_IPC_RMID) {
        pthread_mutex_lock(&shm_lock);
        s = find_by_shmid(shmid);
        /*
         * SysV IPC_RMID only *marks* a segment destroyed: it must stay
         * usable until the last attacher detaches. The host kernel already
         * honours that, so forward the call — but do NOT tear down our own
         * mapping while the guest is still attached.
         *
         * GTK+ 1.2 (gdk/gdkimage.c) RMIDs immediately after XShmAttach and
         * then writes the image pixels. Releasing the stolen Stage-2 slice
         * here silently redirected those writes into ordinary guest RAM,
         * so the X server read a zero-filled segment — every XMMS skin
         * pixmap came out black, on both the AppKit and Xplugin paths.
         */
        int rmid_rc = shmctl(shmid, IPC_RMID, NULL);
        if (rmid_rc < 0 && !s) {
            /* Propagate the real error (EINVAL/EPERM) instead of always
             * reporting success. */
            int e = errno;
            pthread_mutex_unlock(&shm_lock);
            errno = e;
            return linux_errno();
        }
        if (s) {
            if (s->nattach > 0)
                s->rmid_pending = 1; /* freed by the last sys_shmdt */
            else
                shm_release_locked(g, s);
        }
        pthread_mutex_unlock(&shm_lock);
        return 0;
    }

    if (cmd == LINUX_IPC_STAT || cmd == LINUX_SHM_STAT) {
        if (buf_gva == 0)
            return -LINUX_EFAULT;
        int target = shmid;
        int ret_id = 0;
        if (cmd == LINUX_SHM_STAT) {
            /* SHM_STAT's argument is an INDEX into the kernel's table and
             * the call returns the segment id — it was being treated as a
             * shmid, so it always reported 0. */
            if (shmid < 0 || shmid >= HL_SHM_MAX) return -LINUX_EINVAL;
            pthread_mutex_lock(&shm_lock);
            if (!segs[shmid].used) {
                pthread_mutex_unlock(&shm_lock);
                return -LINUX_EINVAL;
            }
            target = segs[shmid].host_shmid;
            pthread_mutex_unlock(&shm_lock);
            ret_id = target;
        }
        if (shmctl(target, IPC_STAT, &ds) < 0)
            return linux_errno();
        memset(&lds, 0, sizeof(lds));
        lds.shm_segsz = ds.shm_segsz;
        lds.shm_nattch = (uint64_t)ds.shm_nattch;
        if (guest_write(g, buf_gva, &lds, sizeof(lds)) < 0)
            return -LINUX_EFAULT;
        return ret_id;
    }

    if (cmd == LINUX_IPC_INFO) {
        /* Previously returned 0 without writing anything, so `ipcs -l`
         * read its own uninitialized stack as the limits. */
        if (buf_gva == 0) return -LINUX_EFAULT;
        linux_shminfo_t si;
        memset(&si, 0, sizeof(si));
        si.shmmax = (uint64_t)HL_SHM_2MB * 512;   /* 1GB */
        si.shmmin = 1;
        si.shmmni = HL_SHM_MAX;
        si.shmseg = HL_SHM_MAX;
        si.shmall = si.shmmax / 4096;
        if (guest_write(g, buf_gva, &si, sizeof(si)) < 0)
            return -LINUX_EFAULT;
        /* Linux returns the highest used index. */
        int high = 0;
        pthread_mutex_lock(&shm_lock);
        for (int i = 0; i < HL_SHM_MAX; i++) if (segs[i].used) high = i;
        pthread_mutex_unlock(&shm_lock);
        return high;
    }

    if (cmd == LINUX_SHM_INFO) {
        if (buf_gva == 0) return -LINUX_EFAULT;
        linux_shm_info_t inf;
        memset(&inf, 0, sizeof(inf));
        int high = 0;
        pthread_mutex_lock(&shm_lock);
        for (int i = 0; i < HL_SHM_MAX; i++) {
            if (!segs[i].used) continue;
            inf.used_ids++;
            inf.shm_tot += segs[i].size / 4096;
            high = i;
        }
        pthread_mutex_unlock(&shm_lock);
        inf.shm_rss = inf.shm_tot;
        if (guest_write(g, buf_gva, &inf, sizeof(inf)) < 0)
            return -LINUX_EFAULT;
        return high;
    }

    if (cmd == LINUX_IPC_SET) {
        /* Permissions are not emulated (single-uid guest), but the call
         * must not fail: it returned EINVAL on a segment the caller owns. */
        if (buf_gva == 0) return -LINUX_EFAULT;
        if (shmctl(shmid, IPC_STAT, &ds) < 0) return linux_errno();
        return 0;
    }

    if (cmd == LINUX_SHM_LOCK || cmd == LINUX_SHM_UNLOCK) {
        /* No swap to lock against; Linux returns 0 for a permitted caller. */
        if (shmctl(shmid, IPC_STAT, &ds) < 0) return linux_errno();
        return 0;
    }

    return -LINUX_EINVAL;
}

/* ---------- fork support ---------- */

int hl_shm_fork_max(void) { return HL_SHM_MAX; }

int
hl_shm_fork_export(hl_shm_fork_rec_t *out, int max)
{
    int n = 0;
    if (!out || max <= 0) return 0;
    pthread_mutex_lock(&shm_lock);
    for (int i = 0; i < HL_SHM_MAX && n < max; i++) {
        /* Export every segment hl knows about, not only the attached ones.
         * shmget() in the parent followed by shmat() in the child is the
         * normal X11/GTK pattern; the child used to reach the "adopt"
         * path, where the shm_cpid == getpid() owner check can never hold
         * (hl forks via posix_spawn, so the child has a different pid) and
         * the attach failed with EACCES. */
        if (!segs[i].used) continue;
        out[n].host_shmid   = segs[i].host_shmid;
        out[n].nattach      = segs[i].nattach;
        out[n].rmid_pending = segs[i].rmid_pending;
        out[n].nattach_va   = segs[i].nattach_va;
        for (int k = 0; k < HL_SHM_FORK_MAX_ATTACH; k++)
            out[n].attach_va[k] = (k < segs[i].nattach_va)
                                ? segs[i].attach_va[k] : 0;
        out[n].size         = segs[i].size;
        out[n].map_size     = segs[i].map_size;
        out[n].ipa_span     = segs[i].ipa_span;
        out[n].guest_va     = segs[i].guest_va;
        out[n].ipa          = segs[i].ipa;
        n++;
    }
    pthread_mutex_unlock(&shm_lock);
    return n;
}

int
hl_shm_fork_import(guest_t *g, const hl_shm_fork_rec_t *in, int n)
{
    if (!in || n <= 0) return 0;
    if (n > HL_SHM_MAX) n = HL_SHM_MAX;

    pthread_mutex_lock(&shm_lock);
    for (int i = 0; i < n; i++) {
        hl_shm_seg_t *s = alloc_slot();
        if (!s) break;

        s->host_shmid   = in[i].host_shmid;
        s->nattach      = in[i].nattach;
        s->rmid_pending = in[i].rmid_pending;
        s->size         = (size_t)in[i].size;
        s->map_size     = (size_t)in[i].map_size;
        s->ipa_span     = (size_t)in[i].ipa_span;
        s->nattach_va   = in[i].nattach_va;
        if (s->nattach_va > HL_SHM_MAX_ATTACH)
            s->nattach_va = HL_SHM_MAX_ATTACH;
        for (int k = 0; k < s->nattach_va; k++)
            s->attach_va[k] = in[i].attach_va[k];
        s->guest_va     = in[i].guest_va;

        /* Known but not attached in the parent: record the metadata only,
         * so the child's own shmat() finds the segment instead of falling
         * into the (correctly restrictive) adopt-a-foreign-shmid path. */
        if (in[i].nattach <= 0 || s->nattach_va == 0) {
            s->ipa = 0;
            s->host_addr = NULL;
            s->stage2_stolen = 0;
            continue;
        }

        /* SysV segments are kernel-global, so the child attaches the very
         * same shmid the parent did — that is what makes the memory
         * genuinely shared rather than copied. */
        void *host = shmat(in[i].host_shmid, NULL, 0);
        if (host == (void *)(intptr_t)-1) {
            memset(s, 0, sizeof(*s));
            continue;
        }

        s->ipa          = in[i].ipa;
        s->host_addr    = host;

        /* Re-establish the Stage-2 override at the same IPA. The guest page
         * tables came across with the memory image and already point here. */
        hv_vm_unmap(s->ipa, s->ipa_span);
        hv_return_t hv = hv_vm_map(host, s->ipa, s->map_size,
                                   HV_MEMORY_READ | HV_MEMORY_WRITE);
        if (hv != HV_SUCCESS) {
            fprintf(stderr,
                    "hl: shm fork import: hv_vm_map ipa=0x%llx sz=0x%zx → %d\n",
                    (unsigned long long)s->ipa, s->map_size, (int)hv);
            hl_shm_restore_stage2(g, s->ipa, s->ipa_span);
            shmdt(host);
            memset(s, 0, sizeof(*s));
            continue;
        }
        s->stage2_stolen = 1;
    }
    pthread_mutex_unlock(&shm_lock);
    hl_shm_ensure_hook();   /* gva_resolve must consult the segment table */
    g->need_tlbi = 1;
    return 0;
}
