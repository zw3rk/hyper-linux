/* proc_emulation.c — /proc and /dev path emulation for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Intercepts openat and readlinkat for /proc, /dev, /etc, and /var/run
 * paths. Returns host fds for synthetic content, or -2 if the path is
 * not intercepted (caller falls through to real syscall).
 */
#include "proc_emulation.h"
#include "syscall_proc.h"    /* proc_get_pid, proc_get_ppid, proc_get_cmdline, proc_get_elf_path */
#include "syscall_internal.h" /* fd_to_host, FD_TABLE_SIZE */
#include "syscall.h"         /* linux_utmpx_t, FD_CLOSED, FD_STDIO, FD_REGULAR */
#include "guest.h"
#include "thread.h"          /* thread_active_count */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <sys/param.h>

/* Create a synthetic file from a buffer. Returns a host fd positioned at
 * the start, or -1 on failure. Caller owns the returned fd.
 * Uses a temp file (unlinked immediately) so that pread/lseek work —
 * rosetta pread()s /proc/self/maps during initialization. */
static int proc_synthetic_fd(const void *data, size_t len) {
    char template[] = "/tmp/hl-proc-XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) return -1;
    unlink(template);  /* Delete on close; fd keeps it alive */

    const uint8_t *p = data;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) { close(fd); return -1; }
        p += n;
        remaining -= n;
    }
    lseek(fd, 0, SEEK_SET);  /* Rewind so first read starts at beginning */
    return fd;
}

int proc_intercept_open(const guest_t *g, const char *path, int linux_flags, int mode) {
    (void)mode;
    (void)linux_flags;

    /* /dev/null -> host /dev/null */
    if (strcmp(path, "/dev/null") == 0) {
        int fd = open("/dev/null", O_RDWR);
        return fd >= 0 ? fd : -1;
    }

    /* /dev/zero -> host /dev/zero */
    if (strcmp(path, "/dev/zero") == 0) {
        int fd = open("/dev/zero", O_RDONLY);
        return fd >= 0 ? fd : -1;
    }

    /* /dev/urandom, /dev/random -> host /dev/urandom */
    if (strcmp(path, "/dev/urandom") == 0 ||
        strcmp(path, "/dev/random") == 0) {
        int fd = open("/dev/urandom", O_RDONLY);
        return fd >= 0 ? fd : -1;
    }

    /* /dev/tty -> host /dev/tty */
    if (strcmp(path, "/dev/tty") == 0) {
        int fd = open("/dev/tty", O_RDWR);
        return fd >= 0 ? fd : -1;
    }

    /* /dev/stdin -> dup(0), /dev/stdout -> dup(1), /dev/stderr -> dup(2) */
    if (strcmp(path, "/dev/stdin") == 0)  return dup(STDIN_FILENO);
    if (strcmp(path, "/dev/stdout") == 0) return dup(STDOUT_FILENO);
    if (strcmp(path, "/dev/stderr") == 0) return dup(STDERR_FILENO);

    /* /dev/fd/N -> dup(N) */
    if (strncmp(path, "/dev/fd/", 8) == 0) {
        int n = atoi(path + 8);
        if (n < 0 || n >= FD_TABLE_SIZE) { errno = EBADF; return -1; }
        int host_fd = fd_to_host(n);
        if (host_fd < 0) { errno = EBADF; return -1; }
        return dup(host_fd);
    }

    /* /proc/self/exe -> open the actual ELF binary.
     * Rosetta opens this to read the x86_64 binary for JIT translation.
     * Unlike readlinkat (which returns the path string), openat needs
     * to return an actual file descriptor to the binary. */
    if (strcmp(path, "/proc/self/exe") == 0) {
        const char *exe = proc_get_elf_path();
        if (!exe) { errno = ENOENT; return -1; }
        int fd = open(exe, O_RDONLY);
        return fd >= 0 ? fd : -1;
    }

    /* /proc/cpuinfo -> synthetic file with CPU count */
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpu < 1) ncpu = 1;
        char buf[4096];
        int off = 0;
        for (int i = 0; i < ncpu && off < (int)sizeof(buf) - 256; i++) {
            off += snprintf(buf + off, sizeof(buf) - off,
                "processor\t: %d\n"
                "BogoMIPS\t: 48.00\n"
                "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics\n"
                "CPU implementer\t: 0x61\n"
                "CPU architecture: 8\n"
                "CPU variant\t: 0x1\n"
                "CPU part\t: 0x022\n"
                "CPU revision\t: 1\n\n", i);
        }
        return proc_synthetic_fd(buf, off);
    }

    /* /proc/self/status -> synthetic process status */
    if (strcmp(path, "/proc/self/status") == 0) {
        /* Compute VmSize from region tracking (total virtual memory) */
        uint64_t vm_size_kb = 0;
        for (int i = 0; i < g->nregions; i++)
            vm_size_kb += (g->regions[i].end - g->regions[i].start);
        vm_size_kb /= 1024;

        /* VmRSS: approximate as non-PROT_NONE regions (we can't query
         * actual residency from HVF, but mapped != PROT_NONE is close) */
        uint64_t vm_rss_kb = 0;
        for (int i = 0; i < g->nregions; i++) {
            if (g->regions[i].prot != 0)  /* PROT_NONE = 0 */
                vm_rss_kb += (g->regions[i].end - g->regions[i].start);
        }
        vm_rss_kb /= 1024;

        int threads = thread_active_count();
        char buf[2048];
        int len = snprintf(buf, sizeof(buf),
            "Name:\thl\n"
            "State:\tR (running)\n"
            "Tgid:\t%lld\n"
            "Pid:\t%lld\n"
            "PPid:\t%lld\n"
            "Uid:\t1000\t1000\t1000\t1000\n"
            "Gid:\t1000\t1000\t1000\t1000\n"
            "VmPeak:\t%llu kB\n"
            "VmSize:\t%llu kB\n"
            "VmRSS:\t%llu kB\n"
            "Threads:\t%d\n",
            (long long)proc_get_pid(),
            (long long)proc_get_pid(),
            (long long)proc_get_ppid(),
            (unsigned long long)vm_size_kb,
            (unsigned long long)vm_size_kb,
            (unsigned long long)vm_rss_kb,
            threads);
        return proc_synthetic_fd(buf, len);
    }

    /* /proc/self/cmdline -> NUL-separated argv */
    if (strcmp(path, "/proc/self/cmdline") == 0) {
        size_t len;
        const char *data = proc_get_cmdline(&len);
        if (!data) return proc_synthetic_fd("", 0);
        return proc_synthetic_fd(data, len);
    }

    /* /proc/self/maps -> generated from guest region tracking.
     * Addresses are page-aligned (rounded down/up) to match real Linux
     * behavior. Rosetta's VMAllocationTracker asserts page alignment.
     *
     * Non-identity mapped regions (via sys_mmap_high_va) are tracked by
     * GPA for gap-finder collision avoidance, but /proc/self/maps must
     * show the VA that was returned to the guest. We use display_va for
     * high-VA regions and va_aliases for rosetta segments.
     *
     * Output merges consecutive regions with the same display_va prefix,
     * prot, and flags into a single maps line. This matches real Linux
     * kernel behavior where a single mmap() call produces one maps entry
     * even when the backing pages span multiple physical frames.
     *
     * Internal regions (shim, shim-data, stack-guard, brk, mmap-rx,
     * mmap-rw) are hidden from /proc/self/maps in rosetta mode. Real
     * Linux VMs only show userspace mappings visible to the ELF loader
     * and runtime. hl's shim/brk/mmap regions are host implementation
     * details that rosetta should not see. */
    if (strcmp(path, "/proc/self/maps") == 0) {
        char buf[16384];
        int off = 0;

        /* Build a flat array of (va_start, va_end, prot, flags, offset,
         * name) from regions[] + preannounced[] with merging. */
        typedef struct {
            uint64_t start, end;
            int prot, flags;
            uint64_t offset;
            char name[64];
        } maps_entry_t;
        maps_entry_t entries[256];
        int nentries = 0;

        /* First pass: convert regions[] to VA-space entries */
        for (int i = 0; i < g->nregions && nentries < 255; i++) {
            const guest_region_t *r = &g->regions[i];
            uint64_t start = r->start;
            uint64_t end = r->end;

            /* Skip internal hl regions in rosetta mode. These are host
             * implementation details not visible in a real Linux VM:
             *   - [shim]: exception vector / EL1 code
             *   - [shim-data]: EL1 stack / data
             *   - [stack-guard]: guard page
             *   - brk/mmap-rx/mmap-rw pre-allocations (no name, low VA,
             *     no display_va — these are hl's address space layout) */
            if (g->is_rosetta) {
                if (r->name[0] == '[' &&
                    (strncmp(r->name, "[shim", 5) == 0 ||
                     strcmp(r->name, "[stack-guard]") == 0))
                    continue;
                /* Skip large pre-allocated regions with no name that
                 * are below the rosetta VA range and not guest-visible.
                 * Keep [stack], [vvar], [vdso] and named regions. */
                if (r->name[0] == '\0' && r->display_va == 0 &&
                    start < 0x100000000ULL)
                    continue;
            }

            /* Resolve display address */
            if (r->display_va != 0) {
                start = r->display_va;
                end = r->display_end ? r->display_end
                                     : (start + (r->end - r->start));
            } else {
                for (int j = 0; j < g->naliases; j++) {
                    const guest_va_alias_t *a = &g->va_aliases[j];
                    if (start >= a->gpa_start &&
                        end <= a->gpa_start + a->size) {
                        uint64_t gpa_off = start - a->gpa_start;
                        start = a->va_start + gpa_off;
                        end = start + (r->end - r->start);
                        break;
                    }
                }
            }

            start &= ~0xFFFULL;
            end = (end + 0xFFF) & ~0xFFFULL;

            /* Try to merge with previous entry if contiguous and
             * same prot/flags/name. This collapses the slab's many
             * 2MB blocks into a single maps line, matching real Linux
             * kernel behavior. */
            if (nentries > 0) {
                maps_entry_t *prev = &entries[nentries - 1];
                if (start == prev->end &&
                    r->prot == prev->prot &&
                    r->flags == prev->flags &&
                    strcmp(r->name, prev->name) == 0) {
                    prev->end = end;
                    continue;
                }
            }

            maps_entry_t *e = &entries[nentries++];
            e->start = start;
            e->end = end;
            e->prot = r->prot;
            e->flags = r->flags;
            e->offset = r->offset;
            if (r->name[0])
                strncpy(e->name, r->name, sizeof(e->name) - 1);
            else
                e->name[0] = '\0';
        }

        /* Add preannounced entries (if any) */
        for (int pi = 0; pi < g->npreannounced && nentries < 255; pi++) {
            const guest_region_t *r = &g->preannounced[pi];
            /* Skip if shadowed by actual regions */
            int shadowed = 0;
            for (int j = 0; j < g->nregions; j++) {
                if (g->regions[j].start < r->end &&
                    g->regions[j].end > r->start) {
                    shadowed = 1;
                    break;
                }
            }
            if (shadowed) continue;
            maps_entry_t *e = &entries[nentries++];
            e->start = r->start & ~0xFFFULL;
            e->end = (r->end + 0xFFF) & ~0xFFFULL;
            e->prot = r->prot;
            e->flags = r->flags;
            e->offset = r->offset;
            if (r->name[0])
                strncpy(e->name, r->name, sizeof(e->name) - 1);
            else
                e->name[0] = '\0';
        }

        /* Sort by start address (regions[] is sorted by GPA but we
         * translated to VA, and rosetta regions are at high VA) */
        for (int i = 1; i < nentries; i++) {
            maps_entry_t tmp = entries[i];
            int j = i - 1;
            while (j >= 0 && entries[j].start > tmp.start) {
                entries[j + 1] = entries[j];
                j--;
            }
            entries[j + 1] = tmp;
        }

        /* Emit formatted output */
        for (int i = 0; i < nentries && off < (int)sizeof(buf) - 256; i++) {
            const maps_entry_t *e = &entries[i];
            char perms[5];
            perms[0] = (e->prot & 0x1) ? 'r' : '-';
            perms[1] = (e->prot & 0x2) ? 'w' : '-';
            perms[2] = (e->prot & 0x4) ? 'x' : '-';
            perms[3] = (e->flags & 0x01) ? 's' : 'p';
            perms[4] = '\0';

            /* Format matches real Linux /proc/<pid>/maps exactly:
             *   %lx-%lx %s %08lx %02x:%02x %lu  <padding>  %s\n
             * Verified against strace of rosetta in a real Lima VZ VM. */
            char line[256];
            int lineoff = snprintf(line, sizeof(line),
                "%llx-%llx %s %08llx 00:00 0",
                (unsigned long long)e->start,
                (unsigned long long)e->end,
                perms,
                (unsigned long long)e->offset);
            if (e->name[0]) {
                while (lineoff < 73 && lineoff < (int)sizeof(line) - 1)
                    line[lineoff++] = ' ';
                lineoff += snprintf(line + lineoff, sizeof(line) - lineoff,
                    "%s", e->name);
            } else {
                if (lineoff < (int)sizeof(line) - 1)
                    line[lineoff++] = ' ';
            }
            off += snprintf(buf + off, sizeof(buf) - off, "%.*s\n",
                            lineoff, line);
        }

        if (g->verbose)
            fprintf(stderr, "hl: /proc/self/maps (%d bytes):\n%.*s", off, off, buf);
        return proc_synthetic_fd(buf, off);
    }

    /* /proc/uptime -> synthetic uptime in seconds.
     * Uses sysctl(KERN_BOOTTIME), same as sys_sysinfo() in syscall_sys.c.
     * Idle time is 0 (no meaningful macOS equivalent). */
    if (strcmp(path, "/proc/uptime") == 0) {
        struct timeval boottime;
        size_t bt_len = sizeof(boottime);
        int mib[2] = { CTL_KERN, KERN_BOOTTIME };
        if (sysctl(mib, 2, &boottime, &bt_len, NULL, 0) < 0)
            return -1;
        struct timeval now;
        gettimeofday(&now, NULL);
        double uptime = (double)(now.tv_sec - boottime.tv_sec)
                      + (double)(now.tv_usec - boottime.tv_usec) / 1e6;
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "%.2f 0.00\n", uptime);
        return proc_synthetic_fd(buf, len);
    }

    /* /proc/loadavg -> synthetic load averages.
     * Musl's getloadavg() reads /proc/loadavg, so GNU uptime needs this. */
    if (strcmp(path, "/proc/loadavg") == 0) {
        double loadavg[3] = {0};
        getloadavg(loadavg, 3);
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "%.2f %.2f %.2f 1/1 %lld\n",
                           loadavg[0], loadavg[1], loadavg[2],
                           (long long)proc_get_pid());
        return proc_synthetic_fd(buf, len);
    }

    /* /var/run/utmp, /run/utmp -> synthetic utmp with current user.
     * Creates one USER_PROCESS record for who, users, pinky. */
    if (strcmp(path, "/var/run/utmp") == 0 ||
        strcmp(path, "/run/utmp") == 0) {
        _Static_assert(sizeof(linux_utmpx_t) == 400,
                       "linux_utmpx_t size mismatch");
        linux_utmpx_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.ut_type = LINUX_USER_PROCESS;
        entry.ut_pid = (int32_t)proc_get_pid();
        strncpy(entry.ut_line, "pts/0", LINUX_UT_LINESIZE);
        strncpy(entry.ut_id, "0", 4);
        const char *user = getenv("USER");
        if (!user) user = "user";
        strncpy(entry.ut_user, user, LINUX_UT_NAMESIZE - 1);
        strncpy(entry.ut_host, "localhost", LINUX_UT_HOSTSIZE - 1);
        struct timeval now;
        gettimeofday(&now, NULL);
        entry.ut_tv_sec = now.tv_sec;
        entry.ut_tv_usec = now.tv_usec;
        return proc_synthetic_fd(&entry, sizeof(entry));
    }

    /* /proc/sys/vm/mmap_min_addr -> synthetic mmap minimum address.
     * Rosetta's VMAllocationTracker reads this to determine the lowest
     * address it can mmap. Real Linux VZ VMs return 32768 (0x8000). */
    if (strcmp(path, "/proc/sys/vm/mmap_min_addr") == 0) {
        const char *data = "32768\n";
        return proc_synthetic_fd(data, strlen(data));
    }

    /* /proc/sys/kernel/randomize_va_space -> ASLR enabled.
     * Rosetta reads this during initialization and uses getrandom()
     * to compute randomized stack/mmap addresses when value is 2.
     * Real Linux VZ VMs return 2 (full ASLR). Returning 0 would
     * cause rosetta to use deterministic addresses that may conflict
     * with hl's fixed memory layout. */
    if (strcmp(path, "/proc/sys/kernel/randomize_va_space") == 0) {
        const char *data = "2\n";
        return proc_synthetic_fd(data, strlen(data));
    }

    /* /proc/version -> synthetic kernel version string */
    if (strcmp(path, "/proc/version") == 0) {
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
            "Linux version 6.1.0-hl (hl@hyper-linux) "
            "(aarch64-linux-musl-gcc) #1 SMP PREEMPT\n");
        return proc_synthetic_fd(buf, len);
    }

    /* /proc/filesystems -> supported filesystem types */
    if (strcmp(path, "/proc/filesystems") == 0) {
        const char *data =
            "\tmpfs\n"
            "\tproc\n"
            "\tsysfs\n"
            "\tdevtmpfs\n"
            "\text4\n"
            "\tvfat\n";
        return proc_synthetic_fd(data, strlen(data));
    }

    /* /proc/mounts, /etc/mtab -> synthetic mount table */
    if (strcmp(path, "/proc/mounts") == 0 ||
        strcmp(path, "/proc/self/mounts") == 0 ||
        strcmp(path, "/proc/self/mountinfo") == 0 ||
        strcmp(path, "/etc/mtab") == 0) {
        char buf[512];
        int len = snprintf(buf, sizeof(buf),
            "/ / ext4 rw,relatime 0 0\n"
            "proc /proc proc rw,nosuid,nodev,noexec 0 0\n"
            "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n"
            "devtmpfs /dev devtmpfs rw,nosuid 0 0\n");
        return proc_synthetic_fd(buf, len);
    }

    /* /proc/self/fd/N -> open the target of the fd (readlink-style) */
    if (strncmp(path, "/proc/self/fd/", 14) == 0) {
        int n = atoi(path + 14);
        if (n < 0 || n >= FD_TABLE_SIZE) { errno = EBADF; return -1; }
        int host_fd = fd_to_host(n);
        if (host_fd < 0) { errno = EBADF; return -1; }
        return dup(host_fd);
    }

    /* /proc/meminfo -> synthetic memory info from host sysctl */
    if (strcmp(path, "/proc/meminfo") == 0) {
        int64_t physmem = 0;
        size_t sz = sizeof(physmem);
        int mib[2] = { CTL_HW, HW_MEMSIZE };
        sysctl(mib, 2, &physmem, &sz, NULL, 0);
        uint64_t total_kb = (uint64_t)physmem / 1024;
        /* Approximate free/available from host vm_statistics if available;
         * for simplicity use ~50% as free (sysinfo does similar). */
        uint64_t free_kb = total_kb / 2;
        uint64_t avail_kb = total_kb * 3 / 4;
        uint64_t buffers_kb = total_kb / 20;
        uint64_t cached_kb = total_kb / 4;
        char buf[2048];
        int len = snprintf(buf, sizeof(buf),
            "MemTotal:       %llu kB\n"
            "MemFree:        %llu kB\n"
            "MemAvailable:   %llu kB\n"
            "Buffers:        %llu kB\n"
            "Cached:         %llu kB\n"
            "SwapCached:     0 kB\n"
            "Active:         %llu kB\n"
            "Inactive:       %llu kB\n"
            "SwapTotal:      0 kB\n"
            "SwapFree:       0 kB\n"
            "Dirty:          0 kB\n"
            "Writeback:      0 kB\n"
            "AnonPages:      %llu kB\n"
            "Mapped:         %llu kB\n"
            "Shmem:          0 kB\n"
            "Slab:           0 kB\n"
            "SReclaimable:   0 kB\n"
            "SUnreclaim:     0 kB\n"
            "KernelStack:    0 kB\n"
            "PageTables:     0 kB\n"
            "CommitLimit:    %llu kB\n"
            "Committed_AS:   0 kB\n"
            "VmallocTotal:   0 kB\n"
            "VmallocUsed:    0 kB\n"
            "VmallocChunk:   0 kB\n",
            (unsigned long long)total_kb,
            (unsigned long long)free_kb,
            (unsigned long long)avail_kb,
            (unsigned long long)buffers_kb,
            (unsigned long long)cached_kb,
            (unsigned long long)(total_kb - free_kb - cached_kb),
            (unsigned long long)(cached_kb / 2),
            (unsigned long long)(total_kb - free_kb - cached_kb - buffers_kb),
            (unsigned long long)(cached_kb / 2),
            (unsigned long long)(total_kb / 2));
        return proc_synthetic_fd(buf, len);
    }

    /* /proc/stat -> synthetic CPU statistics */
    if (strcmp(path, "/proc/stat") == 0) {
        struct timeval boottime;
        size_t bt_len = sizeof(boottime);
        int mib[2] = { CTL_KERN, KERN_BOOTTIME };
        sysctl(mib, 2, &boottime, &bt_len, NULL, 0);
        int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpu < 1) ncpu = 1;
        char buf[4096];
        int off = 0;
        /* Aggregate CPU line (user, nice, system, idle, iowait, irq, softirq) */
        off += snprintf(buf + off, sizeof(buf) - off,
            "cpu  1000 0 500 50000 0 0 0 0 0 0\n");
        /* Per-CPU lines */
        for (int i = 0; i < ncpu && off < (int)sizeof(buf) - 128; i++) {
            off += snprintf(buf + off, sizeof(buf) - off,
                "cpu%d 100 0 50 5000 0 0 0 0 0 0\n", i);
        }
        off += snprintf(buf + off, sizeof(buf) - off,
            "intr 0\n"
            "ctxt 0\n"
            "btime %lld\n"
            "processes 1\n"
            "procs_running 1\n"
            "procs_blocked 0\n",
            (long long)boottime.tv_sec);
        return proc_synthetic_fd(buf, off);
    }

    /* /etc/passwd -> synthetic passwd with root + current user */
    if (strcmp(path, "/etc/passwd") == 0) {
        char buf[512];
        int len = snprintf(buf, sizeof(buf),
            "root:x:0:0:root:/root:/bin/sh\n"
            "user:x:1000:1000:user:/home/user:/bin/sh\n");
        return proc_synthetic_fd(buf, len);
    }

    /* /etc/group -> synthetic group file */
    if (strcmp(path, "/etc/group") == 0) {
        char buf[512];
        int len = snprintf(buf, sizeof(buf),
            "root:x:0:\n"
            "staff:x:20:\n"
            "user:x:1000:\n");
        return proc_synthetic_fd(buf, len);
    }

    return -2;  /* Not intercepted */
}

int proc_intercept_readlink(const char *path, char *buf, size_t bufsiz) {
    /* /proc/self/exe -> path of current ELF binary */
    if (strcmp(path, "/proc/self/exe") == 0) {
        const char *exe = proc_get_elf_path();
        if (!exe) { errno = ENOENT; return -1; }
        size_t len = strlen(exe);
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, exe, len);
        return (int)len;
    }

    /* /proc/self/cwd -> getcwd() */
    if (strcmp(path, "/proc/self/cwd") == 0) {
        char cwd[LINUX_PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return -1;
        size_t len = strlen(cwd);
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, cwd, len);
        return (int)len;
    }

    /* /proc/self/fd/N -> path of host fd (via fcntl F_GETPATH on macOS) */
    if (strncmp(path, "/proc/self/fd/", 14) == 0) {
        int n = atoi(path + 14);
        if (n < 0 || n >= FD_TABLE_SIZE) { errno = EBADF; return -1; }
        int host_fd = fd_to_host(n);
        if (host_fd < 0) { errno = EBADF; return -1; }

        char fdpath[MAXPATHLEN];
        if (fcntl(host_fd, F_GETPATH, fdpath) < 0) { errno = ENOENT; return -1; }
        size_t len = strlen(fdpath);
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, fdpath, len);
        return (int)len;
    }

    return -2;  /* Not intercepted */
}
