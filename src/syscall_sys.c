/* syscall_sys.c — System info and identity syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Uname, getrandom, getcwd, sched_getaffinity, getgroups, getrusage,
 * sysinfo, and prlimit64. All functions are called from syscall_dispatch()
 * in syscall.c.
 */
#include "syscall_sys.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "guest.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/random.h>
#include <stdlib.h>
#include <mach/mach.h>

/* ---------- System info syscall handlers ---------- */

int64_t sys_uname(guest_t *g, uint64_t buf_gva) {
    linux_utsname_t uts;
    memset(&uts, 0, sizeof(uts));
    strncpy(uts.sysname, "Linux", LINUX_UTSNAME_LEN - 1);
    strncpy(uts.nodename, "hl", LINUX_UTSNAME_LEN - 1);
    /* Kernel version must match what Rosetta expects from a VZ guest.
     * Rosetta parses the release string and may enable/disable features
     * based on it. Use a version matching real VZ VMs (Ubuntu 24.04). */
    strncpy(uts.release, "6.8.0-101-generic", LINUX_UTSNAME_LEN - 1);
    strncpy(uts.version, "#101-Ubuntu SMP PREEMPT_DYNAMIC", LINUX_UTSNAME_LEN - 1);
    strncpy(uts.machine, "aarch64", LINUX_UTSNAME_LEN - 1);
    strncpy(uts.domainname, "(none)", LINUX_UTSNAME_LEN - 1);

    if (guest_write(g, buf_gva, &uts, sizeof(uts)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_getrandom(guest_t *g, uint64_t buf_gva, uint64_t buflen,
                      unsigned int flags) {
    (void)flags;
    if (buflen == 0) return 0;

    /* Use a host-side buffer and guest_write for bounds safety.
     * getentropy() works in chunks of 256 bytes max. */
    uint8_t tmp[256];
    uint64_t offset = 0;
    while (offset < buflen) {
        size_t chunk = (buflen - offset) > 256 ? 256 : (size_t)(buflen - offset);
        if (getentropy(tmp, chunk) != 0)
            return linux_errno();
        if (guest_write(g, buf_gva + offset, tmp, chunk) < 0)
            return -LINUX_EFAULT;
        offset += chunk;
    }

    return (int64_t)buflen;
}

int64_t sys_getcwd(guest_t *g, uint64_t buf_gva, uint64_t size) {
    char cwd[LINUX_PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return linux_errno();

    size_t len = strlen(cwd) + 1;
    if (len > size)
        return -LINUX_ERANGE;

    if (guest_write(g, buf_gva, cwd, len) < 0)
        return -LINUX_EFAULT;

    return (int64_t)len;
}

int64_t sys_sched_getaffinity(guest_t *g, int pid, uint64_t size,
                              uint64_t mask_gva) {
    (void)pid;
    /* Single-vCPU model: return a 1-CPU affinity mask.
     * The mask is a bitmask where bit 0 = CPU 0. */
    if (size < 8) return -LINUX_EINVAL;

    uint64_t mask = 1;  /* CPU 0 only */
    if (guest_write(g, mask_gva, &mask, 8) < 0)
        return -LINUX_EFAULT;

    return 8;  /* Returns size of written mask */
}

int64_t sys_getgroups(guest_t *g, int size, uint64_t list_gva) {
    gid_t groups[64];
    int ngroups = getgroups(64, groups);
    if (ngroups < 0) return linux_errno();

    if (size == 0) return ngroups;
    if (size < ngroups) return -LINUX_EINVAL;

    /* Linux uses uint32_t for gid_t on aarch64 */
    uint32_t linux_groups[64];
    for (int i = 0; i < ngroups; i++)
        linux_groups[i] = (uint32_t)groups[i];

    if (guest_write(g, list_gva, linux_groups, ngroups * 4) < 0)
        return -LINUX_EFAULT;

    return ngroups;
}

int64_t sys_getrusage(guest_t *g, int who, uint64_t usage_gva) {
    struct rusage mac_usage;
    if (getrusage(who, &mac_usage) < 0)
        return linux_errno();

    linux_rusage_t lin_usage;
    memset(&lin_usage, 0, sizeof(lin_usage));
    lin_usage.ru_utime.tv_sec  = mac_usage.ru_utime.tv_sec;
    lin_usage.ru_utime.tv_usec = mac_usage.ru_utime.tv_usec;
    lin_usage.ru_stime.tv_sec  = mac_usage.ru_stime.tv_sec;
    lin_usage.ru_stime.tv_usec = mac_usage.ru_stime.tv_usec;
    lin_usage.ru_maxrss  = mac_usage.ru_maxrss;
    lin_usage.ru_minflt  = mac_usage.ru_minflt;
    lin_usage.ru_majflt  = mac_usage.ru_majflt;
    lin_usage.ru_inblock = mac_usage.ru_inblock;
    lin_usage.ru_oublock = mac_usage.ru_oublock;
    lin_usage.ru_nvcsw   = mac_usage.ru_nvcsw;
    lin_usage.ru_nivcsw  = mac_usage.ru_nivcsw;

    if (guest_write(g, usage_gva, &lin_usage, sizeof(lin_usage)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_sysinfo(guest_t *g, uint64_t info_gva) {
    linux_sysinfo_t si;
    memset(&si, 0, sizeof(si));

    /* Uptime from boot time */
    struct timeval boottime;
    size_t bt_len = sizeof(boottime);
    int mib_bt[2] = { CTL_KERN, KERN_BOOTTIME };
    if (sysctl(mib_bt, 2, &boottime, &bt_len, NULL, 0) == 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        si.uptime = now.tv_sec - boottime.tv_sec;
    }

    /* Total RAM: cap to 4GB to match real Linux VZ VM behavior.
     *
     * Rosetta uses sysinfo.totalram to size its JIT slab (the mmap at
     * 0xf00000000000). With the host Mac's full RAM (e.g. 16GB+), rosetta
     * allocates a 384MB slab; the real Lima VZ VM with 4GB reports ~61MB.
     * The different slab size changes rosetta's internal memory layout
     * (hash tables, block arrays, JIT code regions) and can affect JIT
     * code translation behavior. Cap to 4GB to match real VZ VM. */
    uint64_t memsize = 0;
    size_t ms_len = sizeof(memsize);
    int mib_mem[2] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(mib_mem, 2, &memsize, &ms_len, NULL, 0) == 0) {
        const uint64_t VM_RAM_CAP = 4094595072ULL; /* Match Lima VZ 4GB VM */
        si.totalram = (memsize > VM_RAM_CAP) ? VM_RAM_CAP : memsize;
    }

    /* Free RAM from vm_statistics64 (scaled proportionally to capped total) */
    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vmstat, &count) == KERN_SUCCESS) {
        uint64_t page_size = 4096;
        uint64_t real_free = (uint64_t)vmstat.free_count * page_size;
        /* Scale freeram proportionally if we capped totalram */
        if (memsize > 0 && memsize > si.totalram) {
            si.freeram = real_free * si.totalram / memsize;
        } else {
            si.freeram = real_free;
        }
    }

    /* Load averages (× 65536 for fixed-point) */
    double loadavg[3];
    if (getloadavg(loadavg, 3) == 3) {
        si.loads[0] = (uint64_t)(loadavg[0] * 65536.0);
        si.loads[1] = (uint64_t)(loadavg[1] * 65536.0);
        si.loads[2] = (uint64_t)(loadavg[2] * 65536.0);
    }

    si.mem_unit = 1;
    si.procs = 1;  /* Single-process model */

    if (guest_write(g, info_gva, &si, sizeof(si)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

/* ---------- Resource limits ---------- */

/* Translate Linux RLIMIT_* resource numbers to macOS equivalents.
 * The numbering differs: Linux RLIMIT_NPROC=6 vs macOS RLIMIT_NPROC=7. */
static int translate_rlimit_resource(int linux_res) {
    switch (linux_res) {
    case 0:  return RLIMIT_CPU;
    case 1:  return RLIMIT_FSIZE;
    case 2:  return RLIMIT_DATA;
    case 3:  return RLIMIT_STACK;
    case 4:  return RLIMIT_CORE;
    case 5:  return RLIMIT_RSS;
    case 6:  return RLIMIT_NPROC;      /* Linux 6 → macOS 7 */
    case 7:  return RLIMIT_NOFILE;     /* Linux 7 → macOS 8 */
    case 8:  return RLIMIT_MEMLOCK;    /* Linux 8 → macOS 6 */
    case 9:  return RLIMIT_AS;
    default: return -1;
    }
}

int64_t sys_prlimit64(guest_t *g, int pid, int resource,
                      uint64_t new_gva, uint64_t old_gva) {
    (void)pid;  /* Ignore PID; single-process model */

    int mac_res = translate_rlimit_resource(resource);
    if (mac_res < 0) return -LINUX_EINVAL;

    /* Set new limits if requested */
    if (new_gva != 0) {
        linux_rlimit64_t new_lim;
        if (guest_read(g, new_gva, &new_lim, sizeof(new_lim)) < 0)
            return -LINUX_EFAULT;

        struct rlimit rl;
        rl.rlim_cur = new_lim.rlim_cur;
        rl.rlim_max = new_lim.rlim_max;
        if (setrlimit(mac_res, &rl) < 0)
            return linux_errno();
    }

    /* Get current limits if requested */
    if (old_gva != 0) {
        struct rlimit rl;
        if (getrlimit(mac_res, &rl) < 0)
            return linux_errno();

        /* Translate macOS RLIM_INFINITY → Linux RLIM_INFINITY.
         * macOS: 0x7FFFFFFFFFFFFFFF, Linux: 0xFFFFFFFFFFFFFFFF.
         * Rosetta and musl both check for RLIM_INFINITY to determine
         * uncapped resources (e.g., stack size for thread stacks). */
        linux_rlimit64_t old_lim;
        old_lim.rlim_cur = (rl.rlim_cur == RLIM_INFINITY)
                           ? UINT64_MAX : rl.rlim_cur;
        old_lim.rlim_max = (rl.rlim_max == RLIM_INFINITY)
                           ? UINT64_MAX : rl.rlim_max;

        /* RLIMIT_STACK: macOS returns ~8372224 (~8MB-16KB) which differs
         * from Linux's standard 8388608 (8MB = 8192*1024). Rosetta uses
         * this to size its internal stack allocations. Round up to the
         * standard Linux default for consistency. */
        if (resource == 3 /* RLIMIT_STACK */ &&
            old_lim.rlim_cur > 0 && old_lim.rlim_cur < 8388608)
            old_lim.rlim_cur = 8388608;

        if (guest_write(g, old_gva, &old_lim, sizeof(old_lim)) < 0)
            return -LINUX_EFAULT;
    }

    return 0;
}
