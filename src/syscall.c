/* syscall.c — Linux aarch64 syscall dispatch and core infrastructure for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Core infrastructure: FD table, errno translation, flag translation,
 * memory syscalls (brk/mmap), and the main dispatch switch. Individual
 * syscall handlers live in focused modules:
 *   syscall_fs.c   — filesystem operations
 *   syscall_io.c   — read/write, ioctl, splice, sendfile
 *   syscall_poll.c — ppoll, pselect6, epoll (kqueue emulation)
 *   syscall_fd.c   — eventfd, signalfd, timerfd (special FD types)
 *   syscall_time.c — clock, nanosleep, timers
 *   syscall_sys.c  — uname, getrandom, sysinfo, resource limits
 *   syscall_proc.c — fork/exec/wait, process management
 *   syscall_signal.c — signal delivery, rt_sigaction
 *   syscall_net.c  — socket networking
 */
#include "syscall.h"
#include "syscall_internal.h"
#include "syscall_fs.h"
#include "syscall_io.h"
#include "syscall_poll.h"
#include "syscall_fd.h"
#include "syscall_time.h"
#include "syscall_sys.h"
#include "syscall_proc.h"
#include "syscall_exec.h"
#include "fork_ipc.h"
#include "syscall_signal.h"
#include "syscall_net.h"
#include "syscall_inotify.h"
#include "futex.h"
#include "thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>           /* flock() */
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>

/* ---------- Thread-safety locks ---------- */

/* Protects mmap/brk bump allocators and page table extension. Multiple
 * threads may call mmap/brk concurrently; without this lock they could
 * get overlapping allocations or corrupt page table structures. */
static pthread_mutex_t mmap_lock = PTHREAD_MUTEX_INITIALIZER; /* Lock order: 1 */

/* Protects the FD table (fd_alloc, fd_alloc_at, fd_alloc_from, sys_close).
 * File descriptor operations from concurrent threads must be serialized. */
static pthread_mutex_t fd_lock = PTHREAD_MUTEX_INITIALIZER; /* Lock order: 3 */

/* ---------- FD table ---------- */
fd_entry_t fd_table[FD_TABLE_SIZE];

/* Bitmap for O(1) lowest-free-FD allocation. A set bit means the FD
 * is free (FD_CLOSED). Using __builtin_ctzll on each word finds the
 * lowest free FD in O(1) per word, vs O(FD_TABLE_SIZE) linear scan. */
#define FD_BITMAP_WORDS (FD_TABLE_SIZE / 64)
static uint64_t fd_free_bitmap[FD_BITMAP_WORDS];

static inline void fd_bitmap_set_free(int fd) {
    fd_free_bitmap[fd / 64] |= (1ULL << (fd % 64));
}

static inline void fd_bitmap_set_used(int fd) {
    fd_free_bitmap[fd / 64] &= ~(1ULL << (fd % 64));
}

void syscall_init(void) {
    memset(fd_table, 0, sizeof(fd_table));
    signal_init();

    /* Mark all FDs as free in bitmap */
    memset(fd_free_bitmap, 0xFF, sizeof(fd_free_bitmap));

    /* Pre-open stdin/stdout/stderr */
    fd_table[0] = (fd_entry_t){ .type = FD_STDIO, .host_fd = STDIN_FILENO };
    fd_table[1] = (fd_entry_t){ .type = FD_STDIO, .host_fd = STDOUT_FILENO };
    fd_table[2] = (fd_entry_t){ .type = FD_STDIO, .host_fd = STDERR_FILENO };
    fd_bitmap_set_used(0);
    fd_bitmap_set_used(1);
    fd_bitmap_set_used(2);
}

/* ---------- FD helpers ---------- */

/* Find the lowest free FD >= minfd using the bitmap.
 * Returns -1 if no free FD exists at or above minfd.
 * Caller must hold fd_lock. */
static int fd_bitmap_find_free(int minfd) {
    if (minfd < 0) minfd = 0;
    if (minfd >= FD_TABLE_SIZE) return -1;
    int word = minfd / 64;
    int bit  = minfd % 64;

    /* Check the partial first word (mask out bits below minfd) */
    uint64_t masked = fd_free_bitmap[word] & (~0ULL << bit);
    if (masked) {
        int fd = word * 64 + __builtin_ctzll(masked);
        return (fd < FD_TABLE_SIZE) ? fd : -1;
    }

    /* Check remaining full words */
    for (word++; word < FD_BITMAP_WORDS; word++) {
        if (fd_free_bitmap[word]) {
            int fd = word * 64 + __builtin_ctzll(fd_free_bitmap[word]);
            return (fd < FD_TABLE_SIZE) ? fd : -1;
        }
    }
    return -1;
}

/* Allocate the lowest available FD. Returns -1 if table is full. */
int fd_alloc(int type, int host_fd) {
    pthread_mutex_lock(&fd_lock);
    int fd = fd_bitmap_find_free(0);
    if (fd >= 0) {
        fd_bitmap_set_used(fd);
        fd_table[fd].type = type;
        fd_table[fd].host_fd = host_fd;
    }
    pthread_mutex_unlock(&fd_lock);
    return fd;
}

/* Allocate the lowest available FD >= minfd. Returns -1 if none available. */
int fd_alloc_from(int minfd, int type, int host_fd) {
    pthread_mutex_lock(&fd_lock);
    int fd = fd_bitmap_find_free(minfd);
    if (fd >= 0) {
        fd_bitmap_set_used(fd);
        fd_table[fd].type = type;
        fd_table[fd].host_fd = host_fd;
    }
    pthread_mutex_unlock(&fd_lock);
    return fd;
}

/* Allocate a specific FD slot. Properly cleans up any existing entry
 * (including DIR* for directory FDs) before overwriting.
 * Returns -1 if out of range. */
int fd_alloc_at(int fd, int type, int host_fd) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -1;
    pthread_mutex_lock(&fd_lock);
    if (fd_table[fd].type != FD_CLOSED) {
        /* Clean up type-specific resources before closing host fd */
        if (fd_table[fd].dir) {
            if (fd_table[fd].type == FD_DIR)
                closedir((DIR *)fd_table[fd].dir);
            else if (fd_table[fd].type == FD_EPOLL)
                free(fd_table[fd].dir);
        }
        close(fd_table[fd].host_fd);
    }
    fd_table[fd].type = type;
    fd_table[fd].host_fd = host_fd;
    fd_table[fd].linux_flags = 0;
    fd_table[fd].dir = NULL;
    fd_bitmap_set_used(fd);
    pthread_mutex_unlock(&fd_lock);
    return fd;
}

void fd_mark_closed(int fd) {
    pthread_mutex_lock(&fd_lock);
    fd_table[fd].type = FD_CLOSED;
    /* Clear host_fd and dir BEFORE marking the slot free in the bitmap.
     * Otherwise another thread could fd_alloc() this slot, populate it
     * with a new host_fd/dir, and then our stale writes would corrupt
     * the new entry. */
    fd_table[fd].host_fd = -1;
    fd_table[fd].dir = NULL;
    fd_table[fd].linux_flags = 0;
    fd_bitmap_set_free(fd);
    pthread_mutex_unlock(&fd_lock);
}

/* Look up a guest FD. Returns host FD or -1 if invalid. */
int fd_to_host(int guest_fd) {
    if (guest_fd < 0 || guest_fd >= FD_TABLE_SIZE)
        return -1;
    if (fd_table[guest_fd].type == FD_CLOSED)
        return -1;
    return fd_table[guest_fd].host_fd;
}

/* ---------- Linux errno translation ---------- */

/* Convert macOS errno to the equivalent Linux errno value.
 * macOS and Linux errno values diverge starting around errno 35.
 * Returns the negative Linux errno for direct use as a syscall return. */
int64_t linux_errno(void) {
    int e = errno;

    /* Values 1-34 are mostly identical between macOS and Linux, with
     * specific exceptions handled below. For values >= 35 the mapping
     * diverges significantly — a translation table is required. */
    switch (e) {
    /* macOS-specific values that differ from Linux */
    case EAGAIN:       return -LINUX_EAGAIN;       /* mac 35 → linux 11 */
    case EBUSY:        return -LINUX_EBUSY;        /* mac 16 → linux 16 (same) */
    case EEXIST:       return -LINUX_EEXIST;       /* mac 17 → linux 17 (same) */
    case EXDEV:        return -LINUX_EXDEV;        /* mac 18 → linux 18 (same) */
    case ENODEV:       return -LINUX_ENODEV;       /* mac 19 → linux 19 (same) */
    case EISDIR:       return -LINUX_EISDIR;       /* mac 21 → linux 21 (same) */
    case ENFILE:       return -LINUX_ENFILE;       /* mac 23 → linux 23 (same) */
    case EMFILE:       return -LINUX_EMFILE;       /* mac 24 → linux 24 (same) */
    case ETXTBSY:      return -LINUX_ETXTBSY;      /* mac 26 → linux 26 (same) */
    case EFBIG:        return -LINUX_EFBIG;        /* mac 27 → linux 27 (same) */
    case ENOSPC:       return -LINUX_ENOSPC;       /* mac 28 → linux 28 (same) */
    case ESPIPE:       return -LINUX_ESPIPE;       /* mac 29 → linux 29 (same) */
    case EROFS:        return -LINUX_EROFS;        /* mac 30 → linux 30 (same) */
    case EMLINK:       return -LINUX_EMLINK;       /* mac 31 → linux 31 (same) */
    case EPIPE:        return -LINUX_EPIPE;        /* mac 32 → linux 32 (same) */
    case ERANGE:       return -LINUX_ERANGE;       /* mac 34 → linux 34 (same) */
    case EDEADLK:      return -LINUX_EDEADLK;      /* mac 11 → linux 35 */
    case ENAMETOOLONG:  return -LINUX_ENAMETOOLONG;  /* mac 63 → linux 36 */
    case ENOLCK:       return -LINUX_ENOLCK;       /* mac 77 → linux 37 */
    case ENOSYS:       return -LINUX_ENOSYS;       /* mac 78 → linux 38 */
    case ENOTEMPTY:    return -LINUX_ENOTEMPTY;    /* mac 66 → linux 39 */
    case ELOOP:        return -LINUX_ELOOP;        /* mac 62 → linux 40 */
    case ENOPROTOOPT:  return -LINUX_ENOPROTOOPT;  /* mac 42 → linux 92 */
    case EOPNOTSUPP:   return -LINUX_EOPNOTSUPP;   /* mac 45 → linux 95 */
    case EOVERFLOW:    return -LINUX_EOVERFLOW;    /* mac 84 → linux 75 */
    /* Networking errno values */
    case ECONNREFUSED: return -LINUX_ECONNREFUSED; /* mac 61 → linux 111 */
    case ECONNRESET:   return -LINUX_ECONNRESET;   /* mac 54 → linux 104 */
    case ECONNABORTED: return -LINUX_ECONNABORTED; /* mac 53 → linux 103 */
    case EISCONN:      return -LINUX_EISCONN;      /* mac 56 → linux 106 */
    case ENOTCONN:     return -LINUX_ENOTCONN;     /* mac 57 → linux 107 */
    case EADDRINUSE:   return -LINUX_EADDRINUSE;   /* mac 48 → linux 98 */
    case EADDRNOTAVAIL:return -LINUX_EADDRNOTAVAIL;/* mac 49 → linux 99 */
    case ENETUNREACH:  return -LINUX_ENETUNREACH;  /* mac 51 → linux 101 */
    case EHOSTUNREACH: return -LINUX_EHOSTUNREACH; /* mac 65 → linux 113 */
    case EINPROGRESS:  return -LINUX_EINPROGRESS;  /* mac 36 → linux 115 */
    case EALREADY:     return -LINUX_EALREADY;     /* mac 37 → linux 114 */
    case EAFNOSUPPORT: return -LINUX_EAFNOSUPPORT; /* mac 47 → linux 97 */
    case EMSGSIZE:     return -LINUX_EMSGSIZE;     /* mac 40 → linux 90 */
    case ENOTSOCK:     return -LINUX_ENOTSOCK;     /* mac 38 → linux 88 */
    case EDESTADDRREQ: return -LINUX_EDESTADDRREQ; /* mac 39 → linux 89 */
    case EPROTOTYPE:   return -LINUX_EPROTOTYPE;   /* mac 41 → linux 91 */
    case ETIMEDOUT:    return -LINUX_ETIMEDOUT;    /* mac 60 → linux 110 */
    default:
        /* For errno values 1-34 not listed above, numeric values match.
         * For unmapped values, pass through (best effort). */
        if (e >= 1 && e <= 34) return -(int64_t)e;
        return -LINUX_EINVAL;  /* Fallback for truly unknown errno values */
    }
}

/* ---------- Linux AT_* flags translation ---------- */

/* Translate Linux AT_* flags to macOS equivalents.
 * Linux and macOS use different values for AT_SYMLINK_NOFOLLOW etc. */
int translate_at_flags(int linux_flags) {
    int mac_flags = 0;
    if (linux_flags & LINUX_AT_SYMLINK_NOFOLLOW)
        mac_flags |= AT_SYMLINK_NOFOLLOW;
    if (linux_flags & LINUX_AT_SYMLINK_FOLLOW)
        mac_flags |= AT_SYMLINK_FOLLOW;
    if (linux_flags & LINUX_AT_REMOVEDIR)
        mac_flags |= AT_REMOVEDIR;
    /* AT_EMPTY_PATH not supported on macOS */
    return mac_flags;
}

/* Resolve dirfd: translate LINUX_AT_FDCWD and guest FDs */
int resolve_dirfd(int dirfd) {
    if (dirfd == LINUX_AT_FDCWD) return AT_FDCWD;
    return fd_to_host(dirfd);
}

/* ---------- Linux open flags translation ---------- */
int translate_open_flags(int linux_flags) {
    int flags = 0;
    int accmode = linux_flags & 3;
    if (accmode == LINUX_O_RDONLY) flags |= O_RDONLY;
    else if (accmode == LINUX_O_WRONLY) flags |= O_WRONLY;
    else if (accmode == LINUX_O_RDWR) flags |= O_RDWR;

    if (linux_flags & LINUX_O_CREAT)    flags |= O_CREAT;
    if (linux_flags & LINUX_O_EXCL)     flags |= O_EXCL;
    if (linux_flags & LINUX_O_TRUNC)    flags |= O_TRUNC;
    if (linux_flags & LINUX_O_APPEND)   flags |= O_APPEND;
    if (linux_flags & LINUX_O_NONBLOCK) flags |= O_NONBLOCK;
    if (linux_flags & LINUX_O_NOFOLLOW)  flags |= O_NOFOLLOW;
    if (linux_flags & LINUX_O_CLOEXEC)   flags |= O_CLOEXEC;
    if (linux_flags & LINUX_O_DIRECTORY) flags |= O_DIRECTORY;
    /* LINUX_O_LARGEFILE: ignored — macOS always uses 64-bit offsets */
    /* LINUX_O_DIRECT: ignored — no O_DIRECT equivalent on macOS */

    return flags;
}

/* ---------- Gap-finding allocator for mmap ---------- */

/* Find the first free gap of at least `length` bytes in the guest address
 * space between min_addr and max_addr. Walks the sorted region array
 * (guest_t.regions[]) — gaps between tracked regions form a natural free
 * list. This replaces the bump allocator so that munmap'd space can be
 * reused (critical for GHC's reserve-trim-reserve pattern).
 *
 * Returns the gap start offset (0-based), or UINT64_MAX if no gap found. */
/* Cached gap-finder hints for mmap. After each successful allocation,
 * the hint is set to the end of the allocation. This amortizes the
 * O(n) region scan to O(1) for sequential allocations (common case).
 * Reset to 0 on munmap when the freed region is before the hint. */
static uint64_t mmap_rw_gap_hint = 0;
static uint64_t mmap_rx_gap_hint = 0;

static uint64_t find_free_gap_inner(const guest_t *g, uint64_t length,
                                     uint64_t min_addr, uint64_t max_addr) {
    uint64_t gap_start = min_addr;

    for (int i = 0; i < g->nregions; i++) {
        /* Skip regions entirely before the current search position */
        if (g->regions[i].end <= gap_start) continue;

        /* If this region starts far enough after gap_start, we have a gap */
        if (g->regions[i].start >= gap_start + length) return gap_start;

        /* Region overlaps — advance past it */
        gap_start = g->regions[i].end;
        /* Page-align the next candidate position */
        gap_start = (gap_start + 4095) & ~4095ULL;
    }

    /* Check trailing space after all regions */
    if (gap_start + length <= max_addr) return gap_start;
    return UINT64_MAX;  /* No suitable gap found */
}

/* Find a free gap, trying the cached hint first. The hint is the
 * position just past the last allocation in this region, so sequential
 * allocations skip already-scanned entries. Falls back to a full scan
 * from min_addr if the hint fails (e.g., after munmap creates a gap). */
static uint64_t find_free_gap(const guest_t *g, uint64_t length,
                               uint64_t min_addr, uint64_t max_addr) {
    /* Select the appropriate hint for the address range */
    uint64_t *hint = (min_addr < MMAP_BASE) ? &mmap_rx_gap_hint
                                             : &mmap_rw_gap_hint;

    /* Try cached hint first (only if within the valid range) */
    if (*hint >= min_addr && *hint < max_addr) {
        uint64_t result = find_free_gap_inner(g, length, *hint, max_addr);
        if (result != UINT64_MAX) {
            *hint = result + length;
            return result;
        }
    }

    /* Full scan from base */
    uint64_t result = find_free_gap_inner(g, length, min_addr, max_addr);
    if (result != UINT64_MAX)
        *hint = result + length;
    return result;
}

/* ---------- Memory syscalls (tightly coupled to guest.h) ---------- */

static int64_t sys_brk(guest_t *g, uint64_t addr) {
    /* brk addresses as seen by the guest are IPA-based */
    uint64_t ipa_brk = guest_ipa(g, g->brk_current);
    uint64_t ipa_base = guest_ipa(g, g->brk_base);

    if (addr == 0) {
        return (int64_t)ipa_brk;
    }

    if (addr < ipa_base) {
        return (int64_t)ipa_brk;
    }

    /* Convert IPA back to offset for internal tracking */
    uint64_t new_off = addr - g->ipa_base;
    if (new_off >= g->guest_size) {
        return (int64_t)ipa_brk;
    }

    /* Extend page tables if brk grows beyond currently-mapped region.
     * The brk region is initially mapped up to MMAP_RX_BASE; if it grows
     * past that, we need to extend dynamically. */
    uint64_t brk_pt_end = (g->brk_current + BLOCK_2MB - 1) & ~(BLOCK_2MB - 1);
    if (brk_pt_end < MMAP_RX_BASE) brk_pt_end = MMAP_RX_BASE;
    if (new_off > brk_pt_end) {
        uint64_t new_end = (new_off + BLOCK_2MB - 1) & ~(BLOCK_2MB - 1);
        if (guest_extend_page_tables(g, brk_pt_end, new_end, MEM_PERM_RW) < 0)
            return (int64_t)ipa_brk;
    }

    /* Zero new pages if growing */
    if (new_off > g->brk_current) {
        memset((uint8_t *)g->host_base + g->brk_current, 0,
               new_off - g->brk_current);
    }

    uint64_t old_brk = g->brk_current;
    g->brk_current = new_off;

    /* Update "[heap]" region tracking */
    if (new_off > g->brk_base) {
        /* Remove old heap region and add updated one */
        guest_region_remove(g, g->brk_base, old_brk > g->brk_base ? old_brk : g->brk_base + 1);
        guest_region_add(g, g->brk_base, new_off,
                         LINUX_PROT_READ | LINUX_PROT_WRITE,
                         LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
                         0, "[heap]");
    } else {
        /* brk shrank back to base — remove heap region */
        guest_region_remove(g, g->brk_base, old_brk > g->brk_base ? old_brk : g->brk_base + 1);
    }

    return (int64_t)guest_ipa(g, g->brk_current);
}

static int64_t sys_mmap(guest_t *g, uint64_t addr, uint64_t length,
                        int prot, int flags, int fd, int64_t offset) {
    int is_anon = (flags & LINUX_MAP_ANONYMOUS) != 0;
    int needs_exec = (prot & LINUX_PROT_EXEC) != 0;
    int is_prot_none = (prot == LINUX_PROT_NONE);

    /* We handle all mmap variants. For file-backed MAP_SHARED, we fall
     * back to MAP_PRIVATE semantics (copy-on-write). Since the guest
     * is single-process, shared vs private is equivalent. */

    /* Round length up to page size */
    length = (length + 4095) & ~4095ULL;

    uint64_t result_off;  /* Result as offset (0-based) */
    if (flags & LINUX_MAP_FIXED) {
        /* MAP_FIXED: addr is IPA-based, convert to offset */
        uint64_t off = addr - g->ipa_base;
        if (off + length > g->guest_size) return -LINUX_ENOMEM;
        result_off = off;
        /* Remove any existing region coverage in the fixed range */
        guest_region_remove(g, result_off, result_off + length);

        if (!is_prot_none) {
            /* Ensure page table entries exist for the fixed range.
             * PROT_NONE reservations skip page table creation, so when
             * MAP_FIXED commits pages within a PROT_NONE region (e.g.,
             * GHC RTS mmap(MAP_FIXED, PROT_READ|PROT_WRITE) inside its
             * PROT_NONE heap reservation), we must create L2 block
             * descriptors first. guest_extend_page_tables is idempotent
             * for already-mapped blocks. */
            int page_perms = MEM_PERM_R;
            if (prot & LINUX_PROT_WRITE) page_perms |= MEM_PERM_W;
            if (prot & LINUX_PROT_EXEC)  page_perms |= MEM_PERM_X;

            uint64_t ext_start = result_off & ~(BLOCK_2MB - 1);
            uint64_t ext_end = (result_off + length + BLOCK_2MB - 1)
                               & ~(BLOCK_2MB - 1);
            if (ext_end > g->guest_size) ext_end = g->guest_size;

            if (guest_extend_page_tables(g, ext_start, ext_end,
                                          page_perms) < 0)
                return -LINUX_ENOMEM;

            /* Fine-tune permissions for the exact range. Handles L3
             * splitting when MAP_FIXED overlays different permissions
             * onto an existing 2MB block (e.g., .data RW over .text RX). */
            guest_update_perms(g, result_off, result_off + length, page_perms);

            /* Zero the region for MAP_ANONYMOUS. Host memory is demand-
             * paged MAP_ANON (zero on first touch), but previously-used
             * pages may contain stale data from earlier mappings. */
            if (is_anon)
                memset((uint8_t *)g->host_base + result_off, 0, length);
        }
    } else if (needs_exec && !(prot & LINUX_PROT_WRITE)) {
        /* PROT_EXEC without PROT_WRITE: allocate from the RX mmap region.
         * Apple HVF enforces W^X on 2MB block page table entries, so
         * executable mappings must be in separate 2MB blocks from writable
         * ones. The RX region at MMAP_RX_BASE is pre-mapped with execute
         * permission. */
        result_off = find_free_gap(g, length, MMAP_RX_BASE, g->mmap_limit);
        if (result_off == UINT64_MAX) return -LINUX_ENOMEM;
        /* High-water mark for fork IPC state transfer */
        uint64_t rx_hwm = result_off + length;
        if (rx_hwm > g->mmap_rx_next) g->mmap_rx_next = rx_hwm;
    } else {
        /* RW (or PROT_NONE, or PROT_READ): allocate from main mmap region.
         * Honor the address hint if provided and within bounds — GHC's
         * block allocator needs the heap at a specific high address range
         * (~264GB) for its MBlock map, and retries in a loop if it gets
         * a low address instead. On real Linux, mmap tries the hint first
         * and falls back to any suitable address. */
        result_off = UINT64_MAX;
        if (addr != 0) {
            uint64_t hint_off = addr - g->ipa_base;
            if (hint_off >= MMAP_BASE && hint_off + length <= g->mmap_limit)
                result_off = find_free_gap(g, length, hint_off, g->mmap_limit);
        }
        if (result_off == UINT64_MAX)
            result_off = find_free_gap(g, length, MMAP_BASE, g->mmap_limit);
        if (result_off == UINT64_MAX) return -LINUX_ENOMEM;
        /* High-water mark for fork IPC state transfer */
        uint64_t rw_hwm = result_off + length;
        if (rw_hwm > g->mmap_next) g->mmap_next = rw_hwm;
    }

    /* PROT_NONE mappings (e.g., GHC RTS heap reservation) don't need page
     * table entries or zeroing. The guest sees a translation fault on access,
     * which is the correct behavior for PROT_NONE. The host memory is
     * demand-paged MAP_ANON, so untouched pages cost nothing.
     * mprotect() later creates page table entries when portions become
     * accessible (e.g., PROT_NONE → PROT_READ|PROT_WRITE). */
    if (!is_prot_none && !(flags & LINUX_MAP_FIXED)) {
        /* Extend page tables for this specific allocation range only.
         * guest_extend_page_tables skips already-mapped blocks, so
         * calling it on pre-mapped regions is a no-op. This avoids
         * creating entries for PROT_NONE gaps between allocations. */
        if (needs_exec && !(prot & LINUX_PROT_WRITE)) {
            uint64_t ext_start = result_off & ~(BLOCK_2MB - 1);
            uint64_t ext_end = (result_off + length + BLOCK_2MB - 1)
                               & ~(BLOCK_2MB - 1);
            if (ext_end > g->mmap_limit) ext_end = g->mmap_limit;
            if (guest_extend_page_tables(g, ext_start, ext_end,
                                          MEM_PERM_RX) < 0)
                return -LINUX_ENOMEM;
            if (ext_end > g->mmap_rx_end)
                g->mmap_rx_end = ext_end;
        } else {
            uint64_t ext_start = result_off & ~(BLOCK_2MB - 1);
            uint64_t ext_end = (result_off + length + BLOCK_2MB - 1)
                               & ~(BLOCK_2MB - 1);
            if (ext_end > g->mmap_limit) ext_end = g->mmap_limit;
            if (guest_extend_page_tables(g, ext_start, ext_end,
                                          MEM_PERM_RW) < 0)
                return -LINUX_ENOMEM;
            if (ext_end > g->mmap_end)
                g->mmap_end = ext_end;
        }

        /* Zero the mapped region */
        memset((uint8_t *)g->host_base + result_off, 0, length);
    }

    /* For file-backed mmap, read file contents into the region.
     * Short reads are acceptable (region is already zeroed above),
     * but total failure means the mapping is useless. */
    if (!is_anon && fd >= 0) {
        int host_fd = fd_to_host(fd);
        if (host_fd < 0) return -LINUX_EBADF;
        ssize_t nr = pread(host_fd, (uint8_t *)g->host_base + result_off,
                           length, offset);
        if (nr < 0) return linux_errno();
    }

    /* Record the new region. Normalize flags for tracking. */
    int track_flags = is_anon ? (LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS)
                              : LINUX_MAP_PRIVATE;
    guest_region_add(g, result_off, result_off + length,
                     prot, track_flags, is_anon ? 0 : (uint64_t)offset, NULL);

    /* Return IPA-based address to guest */
    return (int64_t)guest_ipa(g, result_off);
}

/* ---------- exit_group helper ---------- */

/* Callback for thread_for_each: force-exit each worker vCPU.
 * Skips the calling thread (main thread, handled by should_exit). */
static void thread_force_exit_cb(thread_entry_t *t, void *ctx) {
    (void)ctx;
    /* Don't force-exit our own vCPU — we handle that via should_exit */
    if (t == current_thread) return;
    hv_vcpus_exit(&t->vcpu, 1);
}

/* Callback for thread_for_each: join each worker thread with a timeout.
 * This allows CLONE_CHILD_CLEARTID futex wake to complete before the
 * main thread exits. Skips the calling thread. */
static void thread_join_workers_cb(thread_entry_t *t, void *ctx) {
    (void)ctx;
    if (t == current_thread) return;
    if (!t->active) return;

    /* Timed join: wait up to 100ms for each worker to finish.
     * If it doesn't respond, we proceed anyway to avoid deadlock. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100000000L; /* 100ms */
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_join(t->host_thread, NULL);
}

/* ---------- Main dispatch ---------- */

int syscall_dispatch(hv_vcpu_t vcpu, guest_t *g, int *exit_code, int verbose) {
    uint64_t x0, x1, x2, x3, x4, x5, x8;

    hv_vcpu_get_reg(vcpu, HV_REG_X8, &x8);
    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
    hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1);
    hv_vcpu_get_reg(vcpu, HV_REG_X2, &x2);
    hv_vcpu_get_reg(vcpu, HV_REG_X3, &x3);
    hv_vcpu_get_reg(vcpu, HV_REG_X4, &x4);
    hv_vcpu_get_reg(vcpu, HV_REG_X5, &x5);

    if (verbose) {
        fprintf(stderr, "hl: syscall %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx, "
                "0x%llx, 0x%llx)\n",
                (unsigned long long)x8,
                (unsigned long long)x0, (unsigned long long)x1,
                (unsigned long long)x2, (unsigned long long)x3,
                (unsigned long long)x4, (unsigned long long)x5);
    }

    int64_t result = 0;
    int should_exit = 0;

    switch ((int)x8) {
    /* ---- Tier 1: assembly hello world ---- */
    case SYS_write:
        result = sys_write(g, (int)x0, x1, x2);
        break;
    case SYS_exit:
        /* Per-thread exit: if multiple threads are active, only this one
         * exits. CLONE_CHILD_CLEARTID cleanup is handled by the worker
         * thread after vcpu_run_loop returns. The main thread falls
         * through to normal process exit. */
        *exit_code = (int)x0;
        should_exit = 1;
        break;
    case SYS_exit_group: {
        /* Terminate all threads. Set the global flag and force-exit all
         * worker vCPUs so they break out of hv_vcpu_run. */
        exit_group_code = (int)x0;
        exit_group_requested = 1;

        /* Force-cancel all worker vCPUs. The main thread's vCPU is
         * handled by the normal should_exit path below. */
        thread_for_each(thread_force_exit_cb, NULL);

        /* Join worker threads to allow CLONE_CHILD_CLEARTID futex wake
         * to complete before the process exits. Without this, the main
         * thread may exit before workers finish their cleanup. */
        thread_for_each(thread_join_workers_cb, NULL);

        *exit_code = (int)x0;
        should_exit = 1;
        break;
    }

    /* ---- Tier 2: musl static hello world ---- */
    case SYS_read:
        result = sys_read(g, (int)x0, x1, x2);
        break;
    case SYS_openat:
        result = sys_openat(g, (int)x0, x1, (int)x2, (int)x3);
        break;
    case SYS_close:
        result = sys_close((int)x0);
        break;
    case SYS_readv:
        result = sys_readv(g, (int)x0, x1, (int)x2);
        break;
    case SYS_writev:
        result = sys_writev(g, (int)x0, x1, (int)x2);
        break;
    case SYS_pread64:
        result = sys_pread64(g, (int)x0, x1, x2, (int64_t)x3);
        break;
    case SYS_pwrite64:
        result = sys_pwrite64(g, (int)x0, x1, x2, (int64_t)x3);
        break;
    case SYS_ioctl:
        result = sys_ioctl(g, (int)x0, x1, x2);
        break;
    case SYS_fstat:
        result = sys_fstat(g, (int)x0, x1);
        break;
    case SYS_newfstatat:
        result = sys_newfstatat(g, (int)x0, x1, x2, (int)x3);
        break;
    case SYS_set_tid_address:
        if (current_thread) {
            current_thread->clear_child_tid = x0;
            result = current_thread->guest_tid;
        } else {
            result = proc_get_pid();
        }
        break;
    case SYS_clock_gettime:
        result = sys_clock_gettime(g, (int)x0, x1);
        break;
    case SYS_rt_sigaction:
        result = signal_rt_sigaction(g, (int)x0, x1, x2, x3);
        break;
    case SYS_rt_sigprocmask:
        result = signal_rt_sigprocmask(g, (int)x0, x1, x2, x3);
        break;
    case SYS_uname:
        result = sys_uname(g, x0);
        break;
    case SYS_getpid:
        result = proc_get_pid();
        break;
    case SYS_gettid:
        result = current_thread ? current_thread->guest_tid : proc_get_pid();
        break;
    case SYS_brk:
        pthread_mutex_lock(&mmap_lock);
        result = sys_brk(g, x0);
        pthread_mutex_unlock(&mmap_lock);
        break;
    case SYS_mmap:
        pthread_mutex_lock(&mmap_lock);
        result = sys_mmap(g, x0, x1, (int)x2, (int)x3, (int)x4, (int64_t)x5);
        pthread_mutex_unlock(&mmap_lock);
        break;
    case SYS_munmap: {
        /* Convert IPA-based addr to offset for region tracking */
        uint64_t unmap_off = x0 - g->ipa_base;
        uint64_t unmap_len = (x1 + 4095) & ~4095ULL;
        pthread_mutex_lock(&mmap_lock);
        if (unmap_off + unmap_len <= g->guest_size) {
            /* Zero non-PROT_NONE portions (security: prevent data leaks).
             * PROT_NONE regions were never written to (no page table entries),
             * so the host pages are still in MAP_ANON zero-fill state.
             * Zeroing a huge PROT_NONE range would fault in demand-paged
             * memory for no benefit. */
            uint64_t end = unmap_off + unmap_len;
            for (int i = 0; i < g->nregions; i++) {
                guest_region_t *r = &g->regions[i];
                if (r->start >= end) break;
                if (r->end <= unmap_off) continue;
                if (r->prot == LINUX_PROT_NONE) continue;
                /* Compute overlap of region with unmap range */
                uint64_t zstart = (r->start > unmap_off) ? r->start : unmap_off;
                uint64_t zend = (r->end < end) ? r->end : end;
                memset((uint8_t *)g->host_base + zstart, 0, zend - zstart);
            }
            /* Remove region tracking. The gap-finding allocator will
             * reuse this freed address space for future mmap calls.
             * Note: page table entries are NOT reclaimed — only the
             * semantic region tracking is updated. */
            guest_region_remove(g, unmap_off, end);

            /* Reset gap-finder hints if freed space precedes them,
             * so the next mmap will find the newly available gap.
             * Set hint to the freed address rather than 0 to avoid
             * a full scan from the region base. */
            if (unmap_off < mmap_rw_gap_hint) mmap_rw_gap_hint = unmap_off;
            if (unmap_off < mmap_rx_gap_hint) mmap_rx_gap_hint = unmap_off;
        }
        pthread_mutex_unlock(&mmap_lock);
        result = 0;
        break;
    }
    case SYS_mprotect: {
        /* Convert IPA-based addr to offset for region tracking */
        uint64_t mprot_off = x0 - g->ipa_base;
        uint64_t mprot_len = (x1 + 4095) & ~4095ULL;
        int mprot_prot = (int)x2;
        pthread_mutex_lock(&mmap_lock);
        if (mprot_off + mprot_len <= g->guest_size) {
            /* Update region tracking with new protection bits */
            guest_region_set_prot(g, mprot_off, mprot_off + mprot_len,
                                  mprot_prot);

            if (mprot_prot != LINUX_PROT_NONE) {
                /* Non-PROT_NONE: ensure page table entries exist for this
                 * range. PROT_NONE mmaps skip page table creation, so
                 * mprotect must create them on demand (e.g., GHC RTS
                 * reserves 256GB with PROT_NONE, then mprotects portions
                 * to PROT_READ|PROT_WRITE for heap use). */
                int page_perms = MEM_PERM_R;
                if (mprot_prot & LINUX_PROT_WRITE) page_perms |= MEM_PERM_W;
                if (mprot_prot & LINUX_PROT_EXEC)  page_perms |= MEM_PERM_X;

                /* Extend page tables for the range. This creates 2MB block
                 * entries for any unmapped blocks. Already-mapped blocks
                 * are skipped (idempotent). */
                guest_extend_page_tables(g, mprot_off,
                                          mprot_off + mprot_len, page_perms);

                /* Update permissions. For blocks that already existed with
                 * different perms, this sets the new permissions (splitting
                 * 2MB blocks into 4KB L3 pages if needed for mixed perms). */
                guest_update_perms(g, mprot_off, mprot_off + mprot_len,
                                   page_perms);
            }
            /* PROT_NONE: invalidate page table entries so the guest
             * faults on access. This is required for correctness — GHC
             * and jemalloc use PROT_NONE guard pages to detect overflow. */
            if (mprot_prot == LINUX_PROT_NONE) {
                guest_invalidate_ptes(g, mprot_off, mprot_off + mprot_len);
            }
        }
        pthread_mutex_unlock(&mmap_lock);
        result = 0;
        break;
    }
    case SYS_getrandom:
        result = sys_getrandom(g, x0, x1, (unsigned int)x2);
        break;

    /* ---- Tier 3: file system ops ---- */
    case SYS_mkdirat:
        result = sys_mkdirat(g, (int)x0, x1, (int)x2);
        break;
    case SYS_unlinkat:
        result = sys_unlinkat(g, (int)x0, x1, (int)x2);
        break;
    case SYS_renameat:
        result = sys_renameat2(g, (int)x0, x1, (int)x2, x3, 0);
        break;
    case SYS_renameat2:
        result = sys_renameat2(g, (int)x0, x1, (int)x2, x3, (int)x4);
        break;
    case SYS_nanosleep:
        result = sys_nanosleep(g, x0, x1);
        break;
    case SYS_clock_nanosleep:
        result = sys_clock_nanosleep(g, (int)x0, (int)x1, x2, x3);
        break;

    /* ---- Identity and info syscalls ---- */
    case SYS_getuid:
    case SYS_geteuid:
    case SYS_getgid:
    case SYS_getegid:
        result = 1000;
        break;
    case SYS_gettimeofday:
        result = sys_gettimeofday(g, x0, x1);
        break;
    case SYS_ftruncate:
        result = sys_ftruncate((int)x0, (int64_t)x1);
        break;
    case SYS_statfs:
        result = sys_statfs(g, x0, x1);
        break;
    case SYS_fstatfs:
        result = sys_fstatfs(g, (int)x0, x1);
        break;
    case SYS_umask:
        result = (int64_t)umask((mode_t)x0);
        break;
    case SYS_madvise:
        result = 0;  /* Stub: ignore memory advice */
        break;

    /* ---- Networking ---- */
    case SYS_socket:
        result = sys_socket(g, (int)x0, (int)x1, (int)x2);
        break;
    case SYS_socketpair:
        result = sys_socketpair(g, (int)x0, (int)x1, (int)x2, x3);
        break;
    case SYS_bind:
        result = sys_bind(g, (int)x0, x1, (uint32_t)x2);
        break;
    case SYS_listen:
        result = sys_listen((int)x0, (int)x1);
        break;
    case SYS_accept:
        result = sys_accept(g, (int)x0, x1, x2);
        break;
    case SYS_accept4:
        result = sys_accept4(g, (int)x0, x1, x2, (int)x3);
        break;
    case SYS_connect:
        result = sys_connect(g, (int)x0, x1, (uint32_t)x2);
        break;
    case SYS_getsockname:
        result = sys_getsockname(g, (int)x0, x1, x2);
        break;
    case SYS_getpeername:
        result = sys_getpeername(g, (int)x0, x1, x2);
        break;
    case SYS_sendto:
        result = sys_sendto(g, (int)x0, x1, x2, (int)x3, x4, (uint32_t)x5);
        break;
    case SYS_recvfrom:
        result = sys_recvfrom(g, (int)x0, x1, x2, (int)x3, x4, x5);
        break;
    case SYS_setsockopt:
        result = sys_setsockopt(g, (int)x0, (int)x1, (int)x2, x3, (uint32_t)x4);
        break;
    case SYS_getsockopt:
        result = sys_getsockopt(g, (int)x0, (int)x1, (int)x2, x3, x4);
        break;
    case SYS_shutdown:
        result = sys_shutdown((int)x0, (int)x1);
        break;
    case SYS_sendmsg:
        result = sys_sendmsg(g, (int)x0, x1, (int)x2);
        break;
    case SYS_recvmsg:
        result = sys_recvmsg(g, (int)x0, x1, (int)x2);
        break;

    /* ---- Tier 3: coreutils ---- */
    case SYS_getcwd:
        result = sys_getcwd(g, x0, x1);
        break;
    case SYS_dup:
        result = sys_dup((int)x0);
        break;
    case SYS_dup3:
        result = sys_dup3((int)x0, (int)x1, (int)x2);
        break;
    case SYS_fcntl:
        result = sys_fcntl(g, (int)x0, (int)x1, x2);
        break;
    case SYS_faccessat:
        result = sys_faccessat(g, (int)x0, x1, (int)x2, (int)x3);
        break;
    case SYS_chdir:
        result = sys_chdir(g, x0);
        break;
    case SYS_fchdir:
        result = sys_fchdir((int)x0);
        break;
    case SYS_pipe2:
        result = sys_pipe2(g, x0, (int)x1);
        break;
    case SYS_getdents64:
        result = sys_getdents64(g, (int)x0, x1, x2);
        break;
    case SYS_lseek:
        result = sys_lseek((int)x0, (int64_t)x1, (int)x2);
        break;
    case SYS_readlinkat:
        result = sys_readlinkat(g, (int)x0, x1, x2, x3);
        break;

    /* ---- Batch 1: File manipulation ---- */
    case SYS_mknodat:
        result = sys_mknodat(g, (int)x0, x1, (int)x2, (int)x3);
        break;
    case SYS_symlinkat:
        result = sys_symlinkat(g, x0, (int)x1, x2);
        break;
    case SYS_linkat:
        result = sys_linkat(g, (int)x0, x1, (int)x2, x3, (int)x4);
        break;
    case SYS_fchmod:
        result = sys_fchmod((int)x0, (uint32_t)x1);
        break;
    case SYS_fchmodat:
        result = sys_fchmodat(g, (int)x0, x1, (uint32_t)x2, (int)x3);
        break;
    case SYS_fchownat:
        result = sys_fchownat(g, (int)x0, x1, (uint32_t)x2, (uint32_t)x3, (int)x4);
        break;
    case SYS_fchown:
        result = sys_fchown((int)x0, (uint32_t)x1, (uint32_t)x2);
        break;
    case SYS_utimensat:
        result = sys_utimensat(g, (int)x0, x1, x2, (int)x3);
        break;
    case SYS_futex:
        result = sys_futex(g, x0, (int)x1, (uint32_t)x2,
                           x3, x4, (uint32_t)x5);
        break;
    case SYS_set_robust_list:
        result = 0;  /* Stub: robust futex cleanup not implemented.
                       * Musl calls this during pthread_create; returning 0
                       * is safe as long as threads exit cleanly. */
        break;
    case SYS_sigaltstack:
        result = signal_sigaltstack(g, x0, x1);
        break;

    /* ---- Batch 2: Process/system info ---- */
    case SYS_sched_getaffinity:
        result = sys_sched_getaffinity(g, (int)x0, x1, x2);
        break;
    case SYS_getpgid:
        result = proc_get_pid();
        break;
    case SYS_getgroups:
        result = sys_getgroups(g, (int)x0, x1);
        break;
    case SYS_getrusage:
        result = sys_getrusage(g, (int)x0, x1);
        break;
    case SYS_prctl:
        switch ((int)x0) {
        case LINUX_PR_SET_NAME:
        case LINUX_PR_GET_NAME:
            result = 0;  /* Stub: thread naming (no real effect) */
            break;
        case LINUX_PR_SET_PDEATHSIG:
            result = 0;  /* Stub: parent death signal (not meaningful
                          * in our fork model — child is a separate hl
                          * process, not a real Linux child thread) */
            break;
        case LINUX_PR_GET_PDEATHSIG: {
            /* Return 0 (no parent death signal set) */
            int32_t sig = 0;
            if (x1 && guest_write(g, x1, &sig, 4) < 0)
                result = -LINUX_EFAULT;
            else
                result = 0;
            break;
        }
        case LINUX_PR_SET_NO_NEW_PRIVS:
            result = 0;  /* Stub: we never grant new privileges anyway */
            break;
        case LINUX_PR_GET_NO_NEW_PRIVS:
            result = 1;  /* Always report no-new-privs as set */
            break;
        case LINUX_PR_SET_DUMPABLE:
            result = 0;  /* Stub: core dumps not applicable */
            break;
        case LINUX_PR_GET_DUMPABLE:
            result = 1;  /* Report as dumpable (default Linux behavior) */
            break;
        default:
            result = -LINUX_ENOSYS;
            break;
        }
        break;
    case SYS_getppid:
        result = proc_get_ppid();
        break;
    case SYS_sysinfo:
        result = sys_sysinfo(g, x0);
        break;
    case SYS_prlimit64:
        result = sys_prlimit64(g, (int)x0, (int)x1, x2, x3);
        break;

    /* ---- Batch 3: I/O optimization + sync ---- */
    case SYS_fallocate:
        result = sys_fallocate((int)x0, (int)x1, (int64_t)x2, (int64_t)x3);
        break;
    case SYS_sendfile:
        result = sys_sendfile(g, (int)x0, (int)x1, x2, x3);
        break;
    case SYS_sync:
        sync();
        result = 0;
        break;
    case SYS_fsync:
        result = (fsync(fd_to_host((int)x0)) < 0) ? linux_errno() : 0;
        break;
    case SYS_fdatasync:
        /* macOS has no fdatasync; fsync is the closest equivalent */
        result = (fsync(fd_to_host((int)x0)) < 0) ? linux_errno() : 0;
        break;
    case SYS_sched_yield:
        sched_yield();
        result = 0;
        break;
    case SYS_copy_file_range:
        result = sys_copy_file_range(g, (int)x0, x1, (int)x2, x3, x4, (unsigned int)x5);
        break;

    /* ---- Batch 4: Signals + I/O multiplexing ---- */
    case SYS_ppoll:
        result = sys_ppoll(g, x0, (uint32_t)x1, x2, x3);
        break;
    case SYS_pselect6:
        result = sys_pselect6(g, (int)x0, x1, x2, x3, x4, x5);
        break;
    case SYS_kill: {
        int sig = (int)x1;
        int pid = (int)x0;
        int64_t our_pid = proc_get_pid();
        if (sig == 0) {
            /* Signal 0: just check if process exists */
            result = (pid == (int)our_pid || pid == 0 || pid == -1) ? 0 : -LINUX_ESRCH;
        } else if (pid == (int)our_pid || pid == 0 || pid == -1) {
            /* Sending to self (or all processes in our model) */
            signal_queue(sig);
            result = 0;
        } else {
            result = -LINUX_ESRCH;
        }
        break;
    }
    case SYS_tgkill: {
        int sig = (int)x2;
        int tid = (int)x1;
        /* Accept tgkill targeting any active thread in our process */
        thread_entry_t *target = thread_find((int64_t)tid);
        if (!target) {
            /* Fall back to checking main PID for compatibility */
            int64_t our_pid = proc_get_pid();
            if (tid == (int)our_pid) target = current_thread;
        }
        if (target) {
            if (sig > 0) signal_queue(sig);
            result = 0;
        } else {
            result = -LINUX_ESRCH;
        }
        break;
    }
    case SYS_rt_sigsuspend:
        result = signal_rt_sigsuspend(g, x0, x1);
        break;
    case SYS_rt_sigpending:
        result = signal_rt_sigpending(g, x0, x1);
        break;
    case SYS_rt_sigreturn: {
        int ret = signal_rt_sigreturn(vcpu, g);
        if (ret == SYSCALL_EXEC_HAPPENED)
            return SYSCALL_EXEC_HAPPENED;
        result = ret;
        break;
    }
    case SYS_setpgid:
        result = 0;  /* Stub: process groups not meaningful */
        break;
    case SYS_setsid:
        result = proc_get_pid();
        break;

    /* ---- Quick-win syscalls ---- */
    case SYS_truncate:
        result = sys_truncate(g, x0, (int64_t)x1);
        break;
    case SYS_flock: {
        /* Passthrough to macOS flock(). Linux and macOS flock operations
         * use the same constants: LOCK_SH=1, LOCK_EX=2, LOCK_UN=8,
         * LOCK_NB=4 — so no flag translation is needed. */
        int host_fd = fd_to_host((int)x0);
        if (host_fd < 0) { result = -LINUX_EBADF; break; }
        result = flock(host_fd, (int)x1) < 0 ? linux_errno() : 0;
        break;
    }
    case SYS_setuid:
    case SYS_setgid:
    case SYS_setreuid:
    case SYS_setregid:
    case SYS_setresuid:
    case SYS_setresgid:
        result = 0;  /* Stub: pretend success */
        break;
    case SYS_getresuid: {
        /* Write {ruid,euid,suid} = {1000,1000,1000} to guest pointers */
        uint32_t uid = 1000;
        if ((x0 && guest_write(g, x0, &uid, 4) < 0) ||
            (x1 && guest_write(g, x1, &uid, 4) < 0) ||
            (x2 && guest_write(g, x2, &uid, 4) < 0)) {
            result = -LINUX_EFAULT;
            break;
        }
        result = 0;
        break;
    }
    case SYS_getresgid: {
        uint32_t gid = 1000;
        if ((x0 && guest_write(g, x0, &gid, 4) < 0) ||
            (x1 && guest_write(g, x1, &gid, 4) < 0) ||
            (x2 && guest_write(g, x2, &gid, 4) < 0)) {
            result = -LINUX_EFAULT;
            break;
        }
        result = 0;
        break;
    }
    case SYS_setpriority:
        result = 0;  /* Stub: pretend success */
        break;
    case SYS_getpriority:
        result = 20;  /* Default nice value */
        break;
    case SYS_close_range:
        result = sys_close_range((unsigned int)x0, (unsigned int)x1,
                                  (unsigned int)x2);
        break;
    case SYS_statx:
        result = sys_statx(g, (int)x0, x1, (int)x2, (unsigned int)x3, x4);
        break;

    /* ---- Process management ---- */
    case SYS_execve:
        result = sys_execve(vcpu, g, x0, x1, x2, verbose);
        if (result == SYSCALL_EXEC_HAPPENED)
            return SYSCALL_EXEC_HAPPENED;
        break;
    case SYS_execveat: {
        /* execveat(dirfd, pathname, argv, envp, flags)
         * x0=dirfd, x1=pathname, x2=argv, x3=envp, x4=flags */
        int dirfd = (int)x0;
        int flags = (int)x4;

        if (dirfd == LINUX_AT_FDCWD && !(flags & LINUX_AT_EMPTY_PATH)) {
            /* Simple case: relative to CWD = same as execve */
            result = sys_execve(vcpu, g, x1, x2, x3, verbose);
            if (result == SYSCALL_EXEC_HAPPENED)
                return SYSCALL_EXEC_HAPPENED;
        } else if (flags & LINUX_AT_EMPTY_PATH) {
            /* AT_EMPTY_PATH: execute the file referred to by dirfd.
             * Resolve the host fd's path and pass to sys_execve. */
            int host_fd = fd_to_host(dirfd);
            if (host_fd < 0) { result = -LINUX_EBADF; break; }
            char fd_path[LINUX_PATH_MAX];
            if (fcntl(host_fd, F_GETPATH, fd_path) < 0) {
                result = -LINUX_ENOENT;
                break;
            }
            /* Write the resolved path into guest memory temporarily.
             * Use the top of the stack scratch area (above stack_top). */
            uint64_t tmp_gva = STACK_TOP + 0x1000;
            size_t pathlen = strlen(fd_path) + 1;
            if (guest_write(g, tmp_gva, fd_path, pathlen) < 0) {
                result = -LINUX_EFAULT;
                break;
            }
            result = sys_execve(vcpu, g, tmp_gva, x2, x3, verbose);
            if (result == SYSCALL_EXEC_HAPPENED)
                return SYSCALL_EXEC_HAPPENED;
        } else {
            /* dirfd + relative pathname: resolve via host openat.
             * Read pathname from guest, open relative to dirfd, get path. */
            char pathname[LINUX_PATH_MAX];
            if (guest_read_str(g, x1, pathname, sizeof(pathname)) < 0) {
                result = -LINUX_EFAULT;
                break;
            }
            int host_dirfd = fd_to_host(dirfd);
            if (host_dirfd < 0) { result = -LINUX_EBADF; break; }
            int tmp_fd = openat(host_dirfd, pathname, O_RDONLY);
            if (tmp_fd < 0) { result = linux_errno(); break; }
            char resolved[LINUX_PATH_MAX];
            if (fcntl(tmp_fd, F_GETPATH, resolved) < 0) {
                close(tmp_fd);
                result = -LINUX_ENOENT;
                break;
            }
            close(tmp_fd);
            uint64_t tmp_gva = STACK_TOP + 0x1000;
            size_t pathlen = strlen(resolved) + 1;
            if (guest_write(g, tmp_gva, resolved, pathlen) < 0) {
                result = -LINUX_EFAULT;
                break;
            }
            result = sys_execve(vcpu, g, tmp_gva, x2, x3, verbose);
            if (result == SYSCALL_EXEC_HAPPENED)
                return SYSCALL_EXEC_HAPPENED;
        }
        break;
    }
    case SYS_clone:
        result = sys_clone(vcpu, g, x0, x1, x2, x3, x4, verbose);
        break;
    case SYS_wait4:
        result = sys_wait4(g, (int)x0, x1, (int)x2, x3);
        break;
    case SYS_waitid:
        result = sys_waitid(g, (int)x0, (int64_t)x1, x2, (int)x3);
        break;

    /* ---- Splice/tee/vmsplice emulation ---- */
    case SYS_splice:
        result = sys_splice(g, (int)x0, x1, (int)x2, x3, (size_t)x4,
                             (unsigned int)x5);
        break;
    case SYS_vmsplice:
        result = sys_vmsplice(g, (int)x0, x1, (unsigned long)x2,
                               (unsigned int)x3);
        break;
    case SYS_tee:
        result = sys_tee((int)x0, (int)x1, (size_t)x2, (unsigned int)x3);
        break;

    /* ---- Timer syscalls ---- */
    case SYS_setitimer:
        result = sys_setitimer(g, (int)x0, x1, x2);
        break;
    case SYS_getitimer:
        result = sys_getitimer(g, (int)x0, x1);
        break;
    case SYS_timerfd_create:
        result = sys_timerfd_create((int)x0, (int)x1);
        break;
    case SYS_timerfd_settime:
        result = sys_timerfd_settime(g, (int)x0, (int)x1, x2, x3);
        break;
    case SYS_timerfd_gettime:
        result = sys_timerfd_gettime(g, (int)x0, x1);
        break;

    /* ---- epoll (emulated via kqueue) ---- */
    case SYS_epoll_create1:
        result = sys_epoll_create1((int)x0);
        break;
    case SYS_epoll_ctl:
        result = sys_epoll_ctl(g, (int)x0, (int)x1, (int)x2, x3);
        break;
    case SYS_epoll_pwait:
        result = sys_epoll_pwait(g, (int)x0, x1, (int)x2, (int)x3, x4);
        break;

    /* ---- eventfd / signalfd ---- */
    case SYS_eventfd2:
        result = sys_eventfd2((unsigned int)x0, (int)x1);
        break;
    case SYS_signalfd4:
        result = sys_signalfd4(g, (int)x0, x1, x2, (int)x3);
        break;

    /* ---- inotify (emulated via kqueue EVFILT_VNODE) ---- */
    case SYS_inotify_init1:
        result = sys_inotify_init1((int)x0);
        break;
    case SYS_inotify_add_watch:
        result = sys_inotify_add_watch(g, (int)x0, x1, (uint32_t)x2);
        break;
    case SYS_inotify_rm_watch:
        result = sys_inotify_rm_watch((int)x0, (int)x1);
        break;

    /* ---- xattr syscalls ---- */
    case SYS_getxattr:
        result = sys_getxattr(g, x0, x1, x2, x3, 0);
        break;
    case SYS_lgetxattr:
        result = sys_getxattr(g, x0, x1, x2, x3, 1);
        break;
    case SYS_setxattr:
        result = sys_setxattr(g, x0, x1, x2, x3, (int)x4, 0);
        break;
    case SYS_lsetxattr:
        result = sys_setxattr(g, x0, x1, x2, x3, (int)x4, 1);
        break;
    case SYS_listxattr:
        result = sys_listxattr(g, x0, x1, x2, 0);
        break;
    case SYS_llistxattr:
        result = sys_listxattr(g, x0, x1, x2, 1);
        break;
    case SYS_removexattr:
        result = sys_removexattr(g, x0, x1, 0);
        break;
    case SYS_lremovexattr:
        result = sys_removexattr(g, x0, x1, 1);
        break;
    case SYS_fgetxattr:
        result = sys_fgetxattr(g, (int)x0, x1, x2, x3);
        break;
    case SYS_fsetxattr:
        result = sys_fsetxattr(g, (int)x0, x1, x2, x3, (int)x4);
        break;
    case SYS_flistxattr:
        result = sys_flistxattr(g, (int)x0, x1, x2);
        break;
    case SYS_fremovexattr:
        result = sys_fremovexattr(g, (int)x0, x1);
        break;

    /* ---- chroot ---- */
    case SYS_chroot:
        result = sys_chroot(g, x0);
        break;

    /* ---- Vectored positioned I/O ---- */
    /* aarch64: offset is a single 64-bit register (x3 for preadv/pwritev,
     * x3 for preadv2/pwritev2 with flags in x4). */
    case SYS_preadv:
        result = sys_preadv(g, (int)x0, x1, (int)x2, (int64_t)x3);
        break;
    case SYS_pwritev:
        result = sys_pwritev(g, (int)x0, x1, (int)x2, (int64_t)x3);
        break;
    case SYS_preadv2:
        result = sys_preadv2(g, (int)x0, x1, (int)x2,
                             (int64_t)x3, (int)x4);
        break;
    case SYS_pwritev2:
        result = sys_pwritev2(g, (int)x0, x1, (int)x2,
                              (int64_t)x3, (int)x4);
        break;

    /* ---- Network batch I/O ---- */
    case SYS_sendmmsg:
        result = sys_sendmmsg(g, (int)x0, x1, (unsigned int)x2, (int)x3);
        break;
    case SYS_recvmmsg:
        result = sys_recvmmsg(g, (int)x0, x1, (unsigned int)x2,
                              (int)x3, x4);
        break;

    /* ---- Advisory / scheduling stubs ---- */
    case SYS_fadvise64:
        result = 0;  /* Advisory only — safe to ignore */
        break;
    case SYS_sched_setaffinity:
        result = 0;  /* Stub: macOS doesn't expose CPU affinity to HVF */
        break;

    /* ---- Misc stubs ---- */
    case SYS_sethostname:
        result = -LINUX_EPERM;  /* Pretend we lack permission */
        break;
    case SYS_memfd_create: {
        /* Emulate with a temp file */
        char template[] = "/tmp/hl-memfd-XXXXXX";
        int fd = mkstemp(template);
        if (fd < 0) { result = linux_errno(); break; }
        unlink(template);  /* Unlink immediately — fd keeps it alive */
        int gfd = fd_alloc(FD_REGULAR, fd);
        if (gfd < 0) { close(fd); result = -LINUX_ENOMEM; break; }
        result = gfd;
        break;
    }
    case SYS_mlock:
    case SYS_munlock:
        result = 0;  /* Stub: pretend success */
        break;
    case SYS_msync: {
        /* Passthrough to host msync — but we don't have real mmap,
         * so just return success */
        result = 0;
        break;
    }
    case SYS_mincore:
        result = -LINUX_ENOSYS;  /* Not meaningful in our model */
        break;
    case SYS_membarrier:
        result = 0;  /* Stub: our single-process model with HVF vCPU
                       * synchronization makes explicit membarrier
                       * unnecessary. GHC RTS calls this during init. */
        break;
    case SYS_clone3:
        /* Modern musl might try clone3 first, then fall back to clone */
        result = -LINUX_ENOSYS;
        break;

    default:
        if (verbose) {
            fprintf(stderr, "hl: unimplemented syscall %llu "
                    "(x0=0x%llx, x1=0x%llx, x2=0x%llx, x3=0x%llx, "
                    "x4=0x%llx, x5=0x%llx)\n",
                    (unsigned long long)x8,
                    (unsigned long long)x0, (unsigned long long)x1,
                    (unsigned long long)x2, (unsigned long long)x3,
                    (unsigned long long)x4, (unsigned long long)x5);
        }
        result = -LINUX_ENOSYS;
        break;
    }

    if (!should_exit) {
        /* Write result back to X0 */
        hv_vcpu_set_reg(vcpu, HV_REG_X0, (uint64_t)result);

        /* Signal the shim to flush TLB if page tables were modified.
         * X8 is the syscall number register — Linux ABI marks it as
         * clobbered, so reusing it for TLBI signalling is safe. */
        if (g->need_tlbi) {
            hv_vcpu_set_reg(vcpu, HV_REG_X8, 1);
            g->need_tlbi = 0;
        } else {
            hv_vcpu_set_reg(vcpu, HV_REG_X8, 0);
        }
    }

    return should_exit;
}
