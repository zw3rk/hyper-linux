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
 * the start, or -1 on failure. Caller owns the returned fd. */
static int proc_synthetic_fd(const void *data, size_t len) {
    /* Use a pipe: write data into write end, return read end */
    int fds[2];
    if (pipe(fds) < 0) return -1;

    /* Write all data (may need multiple writes for large data) */
    const uint8_t *p = data;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fds[1], p, remaining);
        if (n <= 0) { close(fds[0]); close(fds[1]); return -1; }
        p += n;
        remaining -= n;
    }
    close(fds[1]);  /* Close write end so reads see EOF */
    return fds[0];
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
        char buf[1024];
        int len = snprintf(buf, sizeof(buf),
            "Name:\thl\n"
            "State:\tR (running)\n"
            "Tgid:\t%lld\n"
            "Pid:\t%lld\n"
            "PPid:\t%lld\n"
            "Uid:\t1000\t1000\t1000\t1000\n"
            "Gid:\t1000\t1000\t1000\t1000\n",
            (long long)proc_get_pid(),
            (long long)proc_get_pid(),
            (long long)proc_get_ppid());
        return proc_synthetic_fd(buf, len);
    }

    /* /proc/self/cmdline -> NUL-separated argv */
    if (strcmp(path, "/proc/self/cmdline") == 0) {
        size_t len;
        const char *data = proc_get_cmdline(&len);
        if (!data) return proc_synthetic_fd("", 0);
        return proc_synthetic_fd(data, len);
    }

    /* /proc/self/maps -> generated from guest region tracking */
    if (strcmp(path, "/proc/self/maps") == 0) {
        char buf[16384];
        int off = 0;
        for (int i = 0; i < g->nregions && off < (int)sizeof(buf) - 128; i++) {
            const guest_region_t *r = &g->regions[i];
            char perms[5];
            perms[0] = (r->prot & 0x1) ? 'r' : '-'; /* PROT_READ */
            perms[1] = (r->prot & 0x2) ? 'w' : '-'; /* PROT_WRITE */
            perms[2] = (r->prot & 0x4) ? 'x' : '-'; /* PROT_EXEC */
            perms[3] = (r->flags & 0x01) ? 's' : 'p'; /* MAP_SHARED : MAP_PRIVATE */
            perms[4] = '\0';
            /* Format: start-end perms offset dev:major inode pathname */
            off += snprintf(buf + off, sizeof(buf) - off,
                "%08llx-%08llx %s %08llx 00:00 0",
                (unsigned long long)r->start,
                (unsigned long long)r->end,
                perms,
                (unsigned long long)r->offset);
            if (r->name[0]) {
                /* Pad to column 73 (Linux convention) then append name */
                int col = off - (int)(buf + off - buf); /* current position in line */
                (void)col;
                off += snprintf(buf + off, sizeof(buf) - off,
                    "          %s", r->name);
            }
            off += snprintf(buf + off, sizeof(buf) - off, "\n");
        }
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
