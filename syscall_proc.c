/* syscall_proc.c — Process management syscalls for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements process-related syscalls (execve, clone, wait4) and the
 * process table for tracking child processes. Each fork spawns a new
 * host hl process (macOS HVF allows only one VM per process) with
 * IPC state transfer over socketpair.
 */
#include "syscall_proc.h"
#include "syscall_internal.h"
#include "syscall_signal.h"
#include "stack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <mach-o/dyld.h>

/* ---------- HV_CHECK macro (shared with hl.c) ---------- */
#define HV_CHECK(call) do {                                        \
    hv_return_t _r = (call);                                       \
    if (_r != HV_SUCCESS) {                                        \
        fprintf(stderr, "hl: %s failed: %d\n", #call, (int)_r);   \
        exit(1);                                                   \
    }                                                              \
} while (0)

/* ---------- Process state ---------- */

/* Current guest PID and PPID. Initial process is PID=1, PPID=0. */
static int64_t guest_pid = 1;
static int64_t parent_pid = 0;

/* Shim blob reference (set by proc_set_shim from hl.c) */
static const unsigned char *shim_blob_ptr = NULL;
static unsigned int shim_blob_size = 0;

/* Current ELF binary path for /proc/self/exe emulation */
static char elf_path[4096] = {0};

/* Process table for tracking fork children */
static proc_entry_t proc_table[PROC_TABLE_SIZE];
static int64_t next_guest_pid = 2;

/* ---------- Public API ---------- */

void proc_init(void) {
    guest_pid = 1;
    parent_pid = 0;
    next_guest_pid = 2;
    memset(proc_table, 0, sizeof(proc_table));
}

int64_t proc_get_pid(void) {
    return guest_pid;
}

int64_t proc_get_ppid(void) {
    return parent_pid;
}

void proc_set_shim(const unsigned char *blob, unsigned int len) {
    shim_blob_ptr = blob;
    shim_blob_size = len;
}

void proc_set_elf_path(const char *path) {
    if (path) {
        /* Resolve to absolute path if relative */
        if (path[0] == '/') {
            strncpy(elf_path, path, sizeof(elf_path) - 1);
        } else {
            char cwd[4096];
            if (getcwd(cwd, sizeof(cwd))) {
                snprintf(elf_path, sizeof(elf_path), "%s/%s", cwd, path);
            } else {
                strncpy(elf_path, path, sizeof(elf_path) - 1);
            }
        }
        elf_path[sizeof(elf_path) - 1] = '\0';
    } else {
        elf_path[0] = '\0';
    }
}

const char *proc_get_elf_path(void) {
    return elf_path[0] ? elf_path : NULL;
}

/* ---------- /proc and /dev emulation ---------- */

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

int proc_intercept_open(const char *path, int linux_flags, int mode) {
    (void)mode;
    (void)linux_flags;

    /* /dev/null → host /dev/null */
    if (strcmp(path, "/dev/null") == 0) {
        int fd = open("/dev/null", O_RDWR);
        return fd >= 0 ? fd : -1;
    }

    /* /dev/zero → host /dev/zero */
    if (strcmp(path, "/dev/zero") == 0) {
        int fd = open("/dev/zero", O_RDONLY);
        return fd >= 0 ? fd : -1;
    }

    /* /dev/urandom, /dev/random → host /dev/urandom */
    if (strcmp(path, "/dev/urandom") == 0 ||
        strcmp(path, "/dev/random") == 0) {
        int fd = open("/dev/urandom", O_RDONLY);
        return fd >= 0 ? fd : -1;
    }

    /* /dev/tty → host /dev/tty */
    if (strcmp(path, "/dev/tty") == 0) {
        int fd = open("/dev/tty", O_RDWR);
        return fd >= 0 ? fd : -1;
    }

    /* /dev/stdin → dup(0), /dev/stdout → dup(1), /dev/stderr → dup(2) */
    if (strcmp(path, "/dev/stdin") == 0)  return dup(STDIN_FILENO);
    if (strcmp(path, "/dev/stdout") == 0) return dup(STDOUT_FILENO);
    if (strcmp(path, "/dev/stderr") == 0) return dup(STDERR_FILENO);

    /* /dev/fd/N → dup(N) */
    if (strncmp(path, "/dev/fd/", 8) == 0) {
        int n = atoi(path + 8);
        if (n < 0 || n >= FD_TABLE_SIZE) { errno = EBADF; return -1; }
        int host_fd = fd_to_host(n);
        if (host_fd < 0) { errno = EBADF; return -1; }
        return dup(host_fd);
    }

    /* /proc/cpuinfo → synthetic file with CPU count */
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

    /* /proc/self/status → synthetic process status */
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
            (long long)guest_pid,
            (long long)guest_pid,
            (long long)parent_pid);
        return proc_synthetic_fd(buf, len);
    }

    /* /proc/self/maps → empty (minimal) */
    if (strcmp(path, "/proc/self/maps") == 0) {
        return proc_synthetic_fd("", 0);
    }

    /* /proc/uptime → synthetic uptime in seconds.
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

    /* /proc/loadavg → synthetic load averages.
     * Musl's getloadavg() reads /proc/loadavg, so GNU uptime needs this. */
    if (strcmp(path, "/proc/loadavg") == 0) {
        double loadavg[3] = {0};
        getloadavg(loadavg, 3);
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "%.2f %.2f %.2f 1/1 %lld\n",
                           loadavg[0], loadavg[1], loadavg[2],
                           (long long)guest_pid);
        return proc_synthetic_fd(buf, len);
    }

    /* /var/run/utmp, /run/utmp → synthetic utmp with current user.
     * Creates one USER_PROCESS record for who, users, pinky. */
    if (strcmp(path, "/var/run/utmp") == 0 ||
        strcmp(path, "/run/utmp") == 0) {
        _Static_assert(sizeof(linux_utmpx_t) == 400,
                       "linux_utmpx_t size mismatch");
        linux_utmpx_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.ut_type = LINUX_USER_PROCESS;
        entry.ut_pid = (int32_t)guest_pid;
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

    /* /proc/self/fd/N → open the target of the fd (readlink-style) */
    if (strncmp(path, "/proc/self/fd/", 14) == 0) {
        int n = atoi(path + 14);
        if (n < 0 || n >= FD_TABLE_SIZE) { errno = EBADF; return -1; }
        int host_fd = fd_to_host(n);
        if (host_fd < 0) { errno = EBADF; return -1; }
        return dup(host_fd);
    }

    /* /etc/passwd → synthetic passwd with root + current user */
    if (strcmp(path, "/etc/passwd") == 0) {
        char buf[512];
        int len = snprintf(buf, sizeof(buf),
            "root:x:0:0:root:/root:/bin/sh\n"
            "user:x:1000:1000:user:/home/user:/bin/sh\n");
        return proc_synthetic_fd(buf, len);
    }

    /* /etc/group → synthetic group file */
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
    /* /proc/self/exe → path of current ELF binary */
    if (strcmp(path, "/proc/self/exe") == 0) {
        const char *exe = proc_get_elf_path();
        if (!exe) { errno = ENOENT; return -1; }
        size_t len = strlen(exe);
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, exe, len);
        return (int)len;
    }

    /* /proc/self/cwd → getcwd() */
    if (strcmp(path, "/proc/self/cwd") == 0) {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) return -1;
        size_t len = strlen(cwd);
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, cwd, len);
        return (int)len;
    }

    /* /proc/self/fd/N → path of host fd (via fcntl F_GETPATH on macOS) */
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

/* ---------- execve implementation ---------- */

/* Read a NULL-terminated pointer array from guest memory.
 * Each pointer in the array is a 64-bit GVA pointing to a string.
 * Returns the count of entries (excluding the NULL terminator),
 * or -1 on error. Strings are copied into the provided buffer. */
static int read_string_array(guest_t *g, uint64_t array_gva,
                             char **out, int max_count,
                             char *str_buf, size_t str_buf_size) {
    size_t str_off = 0;
    int count = 0;

    for (int i = 0; i < max_count; i++) {
        uint64_t ptr;
        if (guest_read(g, array_gva + i * 8, &ptr, 8) < 0)
            return -1;
        if (ptr == 0) break; /* NULL terminator */

        char *dst = str_buf + str_off;
        size_t remaining = str_buf_size - str_off;
        if (remaining < 2) return -1; /* Buffer full */

        if (guest_read_str(g, ptr, dst, remaining) < 0)
            return -1;

        out[count] = dst;
        str_off += strlen(dst) + 1;
        count++;
    }

    return count;
}

int64_t sys_execve(hv_vcpu_t vcpu, guest_t *g,
                   uint64_t path_gva, uint64_t argv_gva, uint64_t envp_gva,
                   int verbose) {
    /* Step 1: Read path from guest memory */
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    if (verbose)
        fprintf(stderr, "hl: execve(\"%s\")\n", path);

    /* Step 2: Read argv[] and envp[] from guest memory */
    #define MAX_ARGS 256
    #define MAX_ENVS 4096
    #define STR_BUF_SIZE (256 * 1024)

    char *argv[MAX_ARGS + 1];
    char *envp[MAX_ENVS + 1];
    char *argv_buf = malloc(STR_BUF_SIZE);
    char *envp_buf = malloc(STR_BUF_SIZE);
    if (!argv_buf || !envp_buf) {
        free(argv_buf);
        free(envp_buf);
        return -LINUX_ENOMEM;
    }

    int argc = read_string_array(g, argv_gva, argv, MAX_ARGS,
                                  argv_buf, STR_BUF_SIZE);
    if (argc < 0) {
        free(argv_buf); free(envp_buf);
        return -LINUX_EFAULT;
    }
    argv[argc] = NULL;

    int envc = 0;
    if (envp_gva != 0) {
        envc = read_string_array(g, envp_gva, envp, MAX_ENVS,
                                  envp_buf, STR_BUF_SIZE);
        if (envc < 0) {
            free(argv_buf); free(envp_buf);
            return -LINUX_EFAULT;
        }
    }
    envp[envc] = NULL;

    /* Step 3: Verify path is a valid aarch64-linux ELF */
    elf_info_t elf_info;
    if (elf_load(path, &elf_info) < 0) {
        free(argv_buf); free(envp_buf);
        return -LINUX_ENOENT;
    }

    /* Step 4: Close CLOEXEC fds */
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fd_table[i].type != FD_CLOSED &&
            (fd_table[i].linux_flags & LINUX_O_CLOEXEC)) {
            if (fd_table[i].dir) {
                closedir((DIR *)fd_table[i].dir);
                fd_table[i].dir = NULL;
            }
            if (fd_table[i].type != FD_STDIO)
                close(fd_table[i].host_fd);
            fd_table[i].type = FD_CLOSED;
            fd_table[i].host_fd = -1;
            fd_table[i].linux_flags = 0;
        }
    }

    /* Step 5: Reset guest memory (zero ELF, brk, stack, mmap regions) */
    guest_reset(g);

    /* Step 6: Reload shim into guest */
    if (shim_blob_ptr && shim_blob_size > 0) {
        memcpy((uint8_t *)g->host_base + SHIM_BASE,
               shim_blob_ptr, shim_blob_size);
    }

    /* Step 7: Load new ELF segments into guest memory */
    if (elf_map_segments(&elf_info, path, g->host_base, g->guest_size) < 0) {
        fprintf(stderr, "hl: execve: failed to map ELF segments for %s\n", path);
        free(argv_buf); free(envp_buf);
        return -LINUX_ENOEXEC;
    }

    /* Set brk base after the highest loaded segment */
    uint64_t brk_start = (elf_info.load_max + 4095) & ~4095ULL;
    if (brk_start < BRK_BASE_DEFAULT)
        brk_start = BRK_BASE_DEFAULT;
    g->brk_base = brk_start;
    g->brk_current = brk_start;

    /* Step 8: Rebuild page tables */
    #define MAX_REGIONS 16
    mem_region_t regions[MAX_REGIONS];
    int nregions = 0;

    /* Shim code (RX) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = SHIM_BASE,
        .gpa_end   = SHIM_BASE + shim_blob_size,
        .perms     = MEM_PERM_RX
    };

    /* Shim data/stack (RW) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = SHIM_DATA_BASE,
        .gpa_end   = SHIM_DATA_BASE + BLOCK_2MB,
        .perms     = MEM_PERM_RW
    };

    /* ELF segments */
    for (int i = 0; i < elf_info.num_segments; i++) {
        if (nregions >= MAX_REGIONS) break;
        int perms = MEM_PERM_R;
        if (elf_info.segments[i].flags & PF_W) perms |= MEM_PERM_W;
        if (elf_info.segments[i].flags & PF_X) perms |= MEM_PERM_X;
        regions[nregions++] = (mem_region_t){
            .gpa_start = elf_info.segments[i].gpa,
            .gpa_end   = elf_info.segments[i].gpa + elf_info.segments[i].memsz,
            .perms     = perms
        };
    }

    /* brk region (RW) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = g->brk_base,
        .gpa_end   = MMAP_BASE,
        .perms     = MEM_PERM_RW
    };

    /* Stack (RW) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = STACK_BASE,
        .gpa_end   = STACK_TOP,
        .perms     = MEM_PERM_RW
    };

    /* mmap region (RW) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = MMAP_BASE,
        .gpa_end   = MMAP_INITIAL_END,
        .perms     = MEM_PERM_RW
    };
    g->mmap_end = MMAP_INITIAL_END;

    uint64_t ttbr0 = guest_build_page_tables(g, regions, nregions);
    if (!ttbr0) {
        fprintf(stderr, "hl: execve: failed to build page tables\n");
        free(argv_buf); free(envp_buf);
        return -LINUX_ENOMEM;
    }

    /* Step 9: Build new stack with new argv/envp */
    const char **argv_const = (const char **)argv;
    const char **envp_const = (const char **)envp;
    uint64_t sp = build_linux_stack(g, STACK_TOP, argc, argv_const,
                                     envp_const, &elf_info);

    /* Step 10: Set vCPU state for new process */
    uint64_t entry_ipa = guest_ipa(g, elf_info.entry);
    uint64_t sp_ipa    = guest_ipa(g, sp);

    /* Write TTBR0 to vCPU */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, ttbr0);

    /* Set ELR_EL1 to new entry point */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, entry_ipa);

    /* Set SP_EL0 to new stack */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, sp_ipa);

    /* SPSR_EL1: EL0t, AArch64 */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, 0x0);

    /* Zero all general purpose registers */
    for (int i = 0; i < 31; i++) {
        hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, 0);
    }

    /* Tell the shim to invalidate TLB after we rebuilt page tables.
     * Set X8=1 directly because SYSCALL_EXEC_HAPPENED bypasses the
     * normal X8 TLBI signaling in syscall_dispatch(). The shim checks
     * X8 after HVC #5 return: if non-zero, it runs TLBI VMALLE1IS. */
    hv_vcpu_set_reg(vcpu, HV_REG_X8, 1);
    g->need_tlbi = 0;

    if (verbose) {
        fprintf(stderr, "hl: execve: loaded %s, entry=0x%llx sp=0x%llx\n",
                path, (unsigned long long)entry_ipa, (unsigned long long)sp_ipa);
    }

    /* Update ELF path for /proc/self/exe after successful exec */
    proc_set_elf_path(path);

    free(argv_buf);
    free(envp_buf);

    return SYSCALL_EXEC_HAPPENED;
}

/* ---------- IPC Protocol for fork ---------- */

/* Magic values for IPC frame delimiters */
#define IPC_MAGIC_HEADER  0x484C464BU  /* "HLFK" */
#define IPC_MAGIC_SENTINEL 0x484C4F4BU /* "HLOK" */

/* IPC header: sent first over socketpair */
typedef struct {
    uint32_t magic;
    int64_t  child_pid;
    int64_t  parent_pid;
    /* Guest state */
    uint64_t brk_base;
    uint64_t brk_current;
    uint64_t mmap_next;
    uint64_t mmap_end;
    uint64_t pt_pool_next;
    uint64_t ttbr0;
} ipc_header_t;

/* IPC register state */
typedef struct {
    uint64_t elr_el1;
    uint64_t sp_el0;
    uint64_t spsr_el1;
    uint64_t vbar_el1;
    uint64_t ttbr0_el1;
    uint64_t sctlr_el1;
    uint64_t tcr_el1;
    uint64_t mair_el1;
    uint64_t cpacr_el1;
    uint64_t tpidr_el0;
    uint64_t sp_el1;
    uint64_t x[31];
} ipc_registers_t;

/* IPC memory region header */
typedef struct {
    uint64_t offset;
    uint64_t size;
} ipc_region_header_t;

/* IPC FD entry */
typedef struct {
    int32_t guest_fd;
    int32_t type;
    int32_t linux_flags;
    int32_t pad;
} ipc_fd_entry_t;

/* ---------- IPC I/O helpers ---------- */

/* Write exactly len bytes to fd (retry on EINTR/short writes) */
static int ipc_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

/* Read exactly len bytes from fd (retry on EINTR/short reads) */
static int ipc_read_all(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

/* Send file descriptors via SCM_RIGHTS ancillary message */
static int send_fds(int sock, const int *fds, int count) {
    if (count <= 0) return 0;

    /* Send the count first as regular data */
    char dummy = 'F';
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };

    size_t cmsg_size = CMSG_SPACE(count * sizeof(int));
    uint8_t *cmsg_buf = calloc(1, cmsg_size);
    if (!cmsg_buf) return -1;

    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = cmsg_size;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));

    ssize_t ret = sendmsg(sock, &msg, 0);
    free(cmsg_buf);
    return ret < 0 ? -1 : 0;
}

/* Receive file descriptors via SCM_RIGHTS ancillary message */
static int recv_fds(int sock, int *fds, int max_count, int *out_count) {
    char dummy;
    struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };

    size_t cmsg_size = CMSG_SPACE(max_count * sizeof(int));
    uint8_t *cmsg_buf = calloc(1, cmsg_size);
    if (!cmsg_buf) return -1;

    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = cmsg_size;

    ssize_t ret = recvmsg(sock, &msg, 0);
    if (ret < 0) {
        free(cmsg_buf);
        return -1;
    }

    *out_count = 0;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS) {
        int n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (n > max_count) n = max_count;
        memcpy(fds, CMSG_DATA(cmsg), n * sizeof(int));
        *out_count = n;
    }

    free(cmsg_buf);
    return 0;
}

/* ---------- fork_child_main ---------- */

/* Constants needed for vCPU setup (duplicated from hl.c since we can't
 * include shim_blob.h here — the shim is received via IPC) */
#define SCTLR_M   (1ULL << 0)
#define SCTLR_C   (1ULL << 2)
#define SCTLR_I   (1ULL << 12)
#define SCTLR_RES1 ((1ULL << 29) | (1ULL << 28) | (1ULL << 23) | \
                     (1ULL << 22) | (1ULL << 20) | (1ULL << 11) | \
                     (1ULL <<  8) | (1ULL <<  7))

int fork_child_main(int ipc_fd, int verbose, int timeout_sec) {
    /* Step 1: Read IPC header */
    ipc_header_t hdr;
    if (ipc_read_all(ipc_fd, &hdr, sizeof(hdr)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read header\n");
        return 1;
    }
    if (hdr.magic != IPC_MAGIC_HEADER) {
        fprintf(stderr, "hl: fork-child: bad magic 0x%x\n", hdr.magic);
        return 1;
    }

    if (verbose)
        fprintf(stderr, "hl: fork-child: pid=%lld ppid=%lld\n",
                (long long)hdr.child_pid, (long long)hdr.parent_pid);

    /* Set process identity */
    guest_pid = hdr.child_pid;
    parent_pid = hdr.parent_pid;

    /* Step 2: Create guest VM */
    guest_t g;
    if (guest_init(&g, GUEST_MEM_SIZE) < 0) {
        fprintf(stderr, "hl: fork-child: failed to init guest\n");
        return 1;
    }

    /* Restore guest allocation state */
    g.brk_base = hdr.brk_base;
    g.brk_current = hdr.brk_current;
    g.mmap_next = hdr.mmap_next;
    g.mmap_end = hdr.mmap_end;
    g.pt_pool_next = hdr.pt_pool_next;

    /* Step 3: Read registers */
    ipc_registers_t regs;
    if (ipc_read_all(ipc_fd, &regs, sizeof(regs)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read registers\n");
        guest_destroy(&g);
        return 1;
    }

    /* Step 4: Read memory regions */
    uint32_t num_regions;
    if (ipc_read_all(ipc_fd, &num_regions, sizeof(num_regions)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read region count\n");
        guest_destroy(&g);
        return 1;
    }

    if (verbose)
        fprintf(stderr, "hl: fork-child: receiving %u memory regions\n",
                num_regions);

    for (uint32_t i = 0; i < num_regions; i++) {
        ipc_region_header_t rhdr;
        if (ipc_read_all(ipc_fd, &rhdr, sizeof(rhdr)) < 0) {
            fprintf(stderr, "hl: fork-child: failed to read region header\n");
            guest_destroy(&g);
            return 1;
        }

        if (rhdr.offset + rhdr.size > g.guest_size) {
            fprintf(stderr, "hl: fork-child: region out of bounds\n");
            guest_destroy(&g);
            return 1;
        }

        /* Read region data in chunks */
        uint8_t *dst = (uint8_t *)g.host_base + rhdr.offset;
        size_t remaining = rhdr.size;
        while (remaining > 0) {
            size_t chunk = remaining > (1024 * 1024) ? (1024 * 1024) : remaining;
            if (ipc_read_all(ipc_fd, dst, chunk) < 0) {
                fprintf(stderr, "hl: fork-child: failed to read region data\n");
                guest_destroy(&g);
                return 1;
            }
            dst += chunk;
            remaining -= chunk;
        }

        if (verbose)
            fprintf(stderr, "hl: fork-child: region %u: offset=0x%llx size=0x%llx\n",
                    i, (unsigned long long)rhdr.offset,
                    (unsigned long long)rhdr.size);
    }

    /* Step 5: Read FD table */
    uint32_t num_fds;
    if (ipc_read_all(ipc_fd, &num_fds, sizeof(num_fds)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read fd count\n");
        guest_destroy(&g);
        return 1;
    }

    /* Initialize our FD table */
    syscall_init();

    if (num_fds > 0) {
        ipc_fd_entry_t *fd_entries = calloc(num_fds, sizeof(ipc_fd_entry_t));
        if (!fd_entries) {
            guest_destroy(&g);
            return 1;
        }

        if (ipc_read_all(ipc_fd, fd_entries, num_fds * sizeof(ipc_fd_entry_t)) < 0) {
            free(fd_entries);
            guest_destroy(&g);
            return 1;
        }

        /* Receive host FDs via SCM_RIGHTS */
        int *host_fds = calloc(num_fds, sizeof(int));
        if (!host_fds) {
            free(fd_entries);
            guest_destroy(&g);
            return 1;
        }

        int received_count = 0;
        if (recv_fds(ipc_fd, host_fds, (int)num_fds, &received_count) < 0) {
            fprintf(stderr, "hl: fork-child: failed to receive fds\n");
            free(host_fds);
            free(fd_entries);
            guest_destroy(&g);
            return 1;
        }

        /* Populate fd_table */
        for (uint32_t i = 0; i < num_fds; i++) {
            int gfd = fd_entries[i].guest_fd;
            if (gfd < 0 || gfd >= FD_TABLE_SIZE) continue;

            if (fd_entries[i].type == FD_STDIO) {
                /* stdio fds are already set up by syscall_init */
                fd_table[gfd].linux_flags = fd_entries[i].linux_flags;
            } else if ((int)i < received_count) {
                fd_table[gfd].type = fd_entries[i].type;
                fd_table[gfd].host_fd = host_fds[i];
                fd_table[gfd].linux_flags = fd_entries[i].linux_flags;
            }
        }

        free(host_fds);
        free(fd_entries);
    }

    /* Step 6: Read process info (cwd + umask) */
    char cwd[4096];
    uint32_t umask_val;
    if (ipc_read_all(ipc_fd, cwd, sizeof(cwd)) < 0 ||
        ipc_read_all(ipc_fd, &umask_val, sizeof(umask_val)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read process info\n");
        guest_destroy(&g);
        return 1;
    }

    if (cwd[0] != '\0') chdir(cwd);
    umask((mode_t)umask_val);

    /* Step 6b: Read signal state */
    signal_state_t sig;
    if (ipc_read_all(ipc_fd, &sig, sizeof(sig)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read signal state\n");
        guest_destroy(&g);
        return 1;
    }
    signal_set_state(&sig);

    /* Step 6c: Read shim blob (needed for exec in child) */
    uint32_t shim_size;
    if (ipc_read_all(ipc_fd, &shim_size, sizeof(shim_size)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read shim size\n");
        guest_destroy(&g);
        return 1;
    }
    if (shim_size > 0) {
        unsigned char *shim = malloc(shim_size);
        if (!shim) {
            fprintf(stderr, "hl: fork-child: shim alloc failed\n");
            guest_destroy(&g);
            return 1;
        }
        if (ipc_read_all(ipc_fd, shim, shim_size) < 0) {
            fprintf(stderr, "hl: fork-child: failed to read shim blob\n");
            free(shim);
            guest_destroy(&g);
            return 1;
        }
        proc_set_shim(shim, shim_size);
        /* Note: shim memory is leaked intentionally — it must outlive the
         * process since proc_set_shim stores a pointer, not a copy. */
    }

    /* Step 7: Read sentinel */
    uint32_t sentinel;
    if (ipc_read_all(ipc_fd, &sentinel, sizeof(sentinel)) < 0 ||
        sentinel != IPC_MAGIC_SENTINEL) {
        fprintf(stderr, "hl: fork-child: bad sentinel\n");
        guest_destroy(&g);
        return 1;
    }

    /* Close IPC socket */
    close(ipc_fd);

    /* Step 8: Create vCPU and set up registers */
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    HV_CHECK(hv_vcpu_create(&vcpu, &vexit, NULL));
    g.vcpu = vcpu;
    g.exit = vexit;

    /* Restore system registers. For fork children, we enable the MMU
     * directly via hv_vcpu_set_sys_reg (rather than going through the
     * shim entry point) because:
     * 1. The page tables are already set up (copied from parent via IPC)
     * 2. The shim entry zeros ALL GPRs before ERET, which would destroy
     *    callee-saved registers (X19-X28, FP, LR) that the guest expects
     *    preserved across the clone() syscall
     * 3. We can restore the exact parent GPR state and only set X0=0 */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, regs.vbar_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, regs.mair_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, regs.tcr_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, regs.ttbr0_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, regs.cpacr_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, regs.sp_el0));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL1, regs.sp_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, regs.tpidr_el0));

    /* Enable MMU directly (page tables already in guest memory from IPC).
     * SCTLR must include MMU-enable (M), caches (C, I), and RES1 bits. */
    uint64_t sctlr_with_mmu = SCTLR_RES1 | SCTLR_M | SCTLR_C | SCTLR_I;
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, sctlr_with_mmu));

    /* Restore all 31 GPRs from parent state, then override X0=0 (child
     * clone return value). This preserves X1-X30 exactly as they were when
     * the parent called clone(), which is required by the Linux syscall ABI
     * (especially callee-saved X19-X28, FP=X29, LR=X30). */
    for (int i = 0; i < 31; i++)
        HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, regs.x[i]));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, 0)); /* Child gets 0 from clone */

    /* Start at the clone return point in EL0 (not the shim entry).
     * ELR_EL1 points to the guest's clone return site. SPSR_EL1 has
     * the saved EL0 state. We set PC/CPSR for EL0t execution. */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, regs.elr_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, regs.spsr_el1));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, regs.elr_el1));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0)); /* EL0t */

    proc_init();
    /* proc_set_shim was called above from IPC data (step 6c) */

    if (verbose)
        fprintf(stderr, "hl: fork-child: entering vCPU loop\n");

    /* Step 9: Enter vCPU run loop */
    int exit_code = vcpu_run_loop(vcpu, vexit, &g, verbose, timeout_sec);

    guest_destroy(&g);
    return exit_code;
}

/* ---------- sys_clone ---------- */

/* Linux clone flags */
#define LINUX_CLONE_VM       0x00000100
#define LINUX_CLONE_VFORK    0x00004000
#define LINUX_SIGCHLD        17

int64_t sys_clone(hv_vcpu_t vcpu, guest_t *g, uint64_t flags,
                  uint64_t child_stack, uint64_t ptid_gva,
                  uint64_t tls, uint64_t ctid_gva, int verbose) {
    (void)child_stack;
    (void)ptid_gva;
    (void)tls;
    (void)ctid_gva;

    /* We only support fork-like clone (SIGCHLD) and posix_spawn-like
     * clone (CLONE_VM|CLONE_VFORK|SIGCHLD) */
    int is_vfork = (flags & LINUX_CLONE_VFORK) != 0;

    if (verbose)
        fprintf(stderr, "hl: clone(flags=0x%llx, vfork=%d)\n",
                (unsigned long long)flags, is_vfork);

    /* Step 1: Create socketpair for IPC */
    int sock_fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sock_fds) < 0) {
        fprintf(stderr, "hl: clone: socketpair failed: %s\n", strerror(errno));
        return -LINUX_ENOMEM;
    }

    /* Step 2: Find our own executable path */
    char self_path[4096];
    uint32_t path_len = sizeof(self_path);
    if (_NSGetExecutablePath(self_path, &path_len) != 0) {
        fprintf(stderr, "hl: clone: _NSGetExecutablePath failed\n");
        close(sock_fds[0]);
        close(sock_fds[1]);
        return -LINUX_ENOMEM;
    }

    /* Step 3: Spawn child hl process */
    char fd_str[32];
    snprintf(fd_str, sizeof(fd_str), "%d", sock_fds[1]);

    /* Build child argv: [hl_path, [--verbose,] --fork-child, fd, NULL] */
    char *child_argv[6];
    int ci = 0;
    child_argv[ci++] = self_path;
    if (verbose) child_argv[ci++] = "--verbose";
    child_argv[ci++] = "--fork-child";
    child_argv[ci++] = fd_str;
    child_argv[ci] = NULL;

    /* Set up file actions to keep sock_fds[1] open in child */
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);

    /* Set up spawn attributes */
    posix_spawnattr_t spawn_attr;
    posix_spawnattr_init(&spawn_attr);

    extern char **environ;
    pid_t child_host_pid;
    int spawn_ret = posix_spawn(&child_host_pid, self_path, &file_actions,
                                 &spawn_attr, child_argv, environ);
    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&spawn_attr);

    if (spawn_ret != 0) {
        fprintf(stderr, "hl: clone: posix_spawn failed: %s\n",
                strerror(spawn_ret));
        close(sock_fds[0]);
        close(sock_fds[1]);
        return -LINUX_ENOMEM;
    }

    /* Close child's end of socketpair in parent */
    close(sock_fds[1]);
    int ipc_fd = sock_fds[0];

    /* Step 4: Assign guest PID to child */
    int64_t child_guest_pid = next_guest_pid++;

    /* Step 5: Serialize state to child */

    /* Header */
    ipc_header_t hdr = {
        .magic = IPC_MAGIC_HEADER,
        .child_pid = child_guest_pid,
        .parent_pid = guest_pid,
        .brk_base = g->brk_base,
        .brk_current = g->brk_current,
        .mmap_next = g->mmap_next,
        .mmap_end = g->mmap_end,
        .pt_pool_next = g->pt_pool_next,
        .ttbr0 = g->ttbr0,
    };
    if (ipc_write_all(ipc_fd, &hdr, sizeof(hdr)) < 0) {
        fprintf(stderr, "hl: clone: failed to send header\n");
        close(ipc_fd);
        return -LINUX_ENOMEM;
    }

    /* Registers — capture current vCPU state */
    ipc_registers_t regs = {0};
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &regs.elr_el1);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &regs.sp_el0);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, &regs.spsr_el1);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, &regs.vbar_el1);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, &regs.ttbr0_el1);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, &regs.sctlr_el1);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, &regs.tcr_el1);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, &regs.mair_el1);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, &regs.cpacr_el1);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, &regs.tpidr_el0);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL1, &regs.sp_el1);
    for (int i = 0; i < 31; i++)
        hv_vcpu_get_reg(vcpu, HV_REG_X0 + i, &regs.x[i]);

    if (ipc_write_all(ipc_fd, &regs, sizeof(regs)) < 0) {
        fprintf(stderr, "hl: clone: failed to send registers\n");
        close(ipc_fd);
        return -LINUX_ENOMEM;
    }

    /* Memory regions */
    #define MAX_USED_REGIONS 16
    used_region_t used[MAX_USED_REGIONS];
    int nregions = guest_get_used_regions(g, shim_blob_size, used,
                                          MAX_USED_REGIONS);
    uint32_t num_regions = (uint32_t)nregions;
    if (ipc_write_all(ipc_fd, &num_regions, sizeof(num_regions)) < 0) {
        close(ipc_fd);
        return -LINUX_ENOMEM;
    }

    for (int i = 0; i < nregions; i++) {
        ipc_region_header_t rhdr = {
            .offset = used[i].offset,
            .size = used[i].size,
        };
        if (ipc_write_all(ipc_fd, &rhdr, sizeof(rhdr)) < 0) {
            close(ipc_fd);
            return -LINUX_ENOMEM;
        }

        /* Send region data in 1MB chunks */
        uint8_t *src = (uint8_t *)g->host_base + used[i].offset;
        size_t remaining = used[i].size;
        while (remaining > 0) {
            size_t chunk = remaining > (1024 * 1024) ? (1024 * 1024) : remaining;
            if (ipc_write_all(ipc_fd, src, chunk) < 0) {
                close(ipc_fd);
                return -LINUX_ENOMEM;
            }
            src += chunk;
            remaining -= chunk;
        }
    }

    /* FD table — count open fds and send entries + host fds via SCM_RIGHTS */
    int open_fds[FD_TABLE_SIZE];
    ipc_fd_entry_t fd_entries[FD_TABLE_SIZE];
    int host_fds_to_send[FD_TABLE_SIZE];
    uint32_t num_fds = 0;
    int num_host_fds = 0;

    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fd_table[i].type != FD_CLOSED) {
            fd_entries[num_fds].guest_fd = i;
            fd_entries[num_fds].type = fd_table[i].type;
            fd_entries[num_fds].linux_flags = fd_table[i].linux_flags;
            fd_entries[num_fds].pad = 0;

            if (fd_table[i].type != FD_STDIO) {
                /* Dup the fd so child gets its own copy */
                int duped = dup(fd_table[i].host_fd);
                if (duped >= 0) {
                    host_fds_to_send[num_host_fds++] = duped;
                }
            } else {
                /* For stdio, send the actual fd (0, 1, 2) */
                host_fds_to_send[num_host_fds++] = fd_table[i].host_fd;
            }

            open_fds[num_fds] = i;
            num_fds++;
        }
    }

    if (ipc_write_all(ipc_fd, &num_fds, sizeof(num_fds)) < 0) {
        close(ipc_fd);
        return -LINUX_ENOMEM;
    }

    if (num_fds > 0) {
        if (ipc_write_all(ipc_fd, fd_entries, num_fds * sizeof(ipc_fd_entry_t)) < 0) {
            close(ipc_fd);
            return -LINUX_ENOMEM;
        }

        /* Send host FDs via SCM_RIGHTS */
        if (send_fds(ipc_fd, host_fds_to_send, num_host_fds) < 0) {
            fprintf(stderr, "hl: clone: failed to send fds via SCM_RIGHTS\n");
            close(ipc_fd);
            return -LINUX_ENOMEM;
        }

        /* Close duped fds in parent */
        for (int i = 0; i < num_host_fds; i++) {
            if (fd_entries[i].type != FD_STDIO)
                close(host_fds_to_send[i]);
        }
    }

    /* Process info: cwd + umask */
    char cwd[4096] = {0};
    getcwd(cwd, sizeof(cwd));
    mode_t cur_umask = umask(0);
    umask(cur_umask);
    uint32_t umask_val = (uint32_t)cur_umask;

    if (ipc_write_all(ipc_fd, cwd, sizeof(cwd)) < 0 ||
        ipc_write_all(ipc_fd, &umask_val, sizeof(umask_val)) < 0) {
        close(ipc_fd);
        return -LINUX_ENOMEM;
    }

    /* Signal state */
    const signal_state_t *sig = signal_get_state();
    if (ipc_write_all(ipc_fd, sig, sizeof(signal_state_t)) < 0) {
        close(ipc_fd);
        return -LINUX_ENOMEM;
    }

    /* Shim blob (needed for child exec) */
    uint32_t shim_size_u32 = shim_blob_size;
    if (ipc_write_all(ipc_fd, &shim_size_u32, sizeof(shim_size_u32)) < 0) {
        close(ipc_fd);
        return -LINUX_ENOMEM;
    }
    if (shim_blob_size > 0 && shim_blob_ptr) {
        if (ipc_write_all(ipc_fd, shim_blob_ptr, shim_blob_size) < 0) {
            close(ipc_fd);
            return -LINUX_ENOMEM;
        }
    }

    /* Sentinel */
    uint32_t sentinel = IPC_MAGIC_SENTINEL;
    if (ipc_write_all(ipc_fd, &sentinel, sizeof(sentinel)) < 0) {
        close(ipc_fd);
        return -LINUX_ENOMEM;
    }

    close(ipc_fd);

    /* Step 6: Record child in process table */
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (!proc_table[i].active) {
            proc_table[i].active = 1;
            proc_table[i].host_pid = child_host_pid;
            proc_table[i].guest_pid = child_guest_pid;
            proc_table[i].exited = 0;
            proc_table[i].exit_status = 0;
            break;
        }
    }

    /* Step 7: For CLONE_VFORK, wait until child exits or execs.
     * Since we can't detect exec, just wait for child to exit for now.
     * This is correct for posix_spawn: child execs immediately. */
    if (is_vfork) {
        int status;
        waitpid(child_host_pid, &status, 0);

        /* Mark as exited in process table */
        for (int i = 0; i < PROC_TABLE_SIZE; i++) {
            if (proc_table[i].active &&
                proc_table[i].host_pid == child_host_pid) {
                proc_table[i].exited = 1;
                proc_table[i].exit_status = status;
                break;
            }
        }
    }

    if (verbose)
        fprintf(stderr, "hl: clone: child pid=%lld (host=%d)\n",
                (long long)child_guest_pid, child_host_pid);

    return child_guest_pid;
}

/* ---------- sys_wait4 ---------- */

int64_t sys_wait4(guest_t *g, int pid, uint64_t status_gva,
                  int options, uint64_t rusage_gva) {
    (void)rusage_gva; /* Not implemented */

    /* Translate Linux wait options */
    int mac_options = 0;
    if (options & 1) mac_options |= WNOHANG;   /* WNOHANG = 1 on both */
    if (options & 2) mac_options |= WUNTRACED; /* WUNTRACED = 2 on both */

    if (pid == -1) {
        /* Wait for any child. Find any active entry in process table. */
        for (int i = 0; i < PROC_TABLE_SIZE; i++) {
            if (proc_table[i].active) {
                if (proc_table[i].exited) {
                    /* Already reaped (from CLONE_VFORK wait) */
                    int64_t gpid = proc_table[i].guest_pid;
                    if (status_gva) {
                        int32_t linux_status = proc_table[i].exit_status;
                        guest_write(g, status_gva, &linux_status, 4);
                    }
                    proc_table[i].active = 0;
                    return gpid;
                }

                int status;
                pid_t ret = waitpid(proc_table[i].host_pid, &status,
                                     mac_options);
                if (ret > 0) {
                    int64_t gpid = proc_table[i].guest_pid;
                    if (status_gva) {
                        int32_t linux_status = status;
                        guest_write(g, status_gva, &linux_status, 4);
                    }
                    proc_table[i].active = 0;
                    return gpid;
                } else if (ret == 0) {
                    /* WNOHANG and child not yet exited */
                    return 0;
                }
            }
        }
        /* No children */
        return -LINUX_ECHILD;
    }

    /* Wait for specific guest PID */
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (proc_table[i].active && proc_table[i].guest_pid == pid) {
            if (proc_table[i].exited) {
                int64_t gpid = proc_table[i].guest_pid;
                if (status_gva) {
                    int32_t linux_status = proc_table[i].exit_status;
                    guest_write(g, status_gva, &linux_status, 4);
                }
                proc_table[i].active = 0;
                return gpid;
            }

            int status;
            pid_t ret = waitpid(proc_table[i].host_pid, &status,
                                 mac_options);
            if (ret > 0) {
                int64_t gpid = proc_table[i].guest_pid;
                if (status_gva) {
                    int32_t linux_status = status;
                    guest_write(g, status_gva, &linux_status, 4);
                }
                proc_table[i].active = 0;
                /* Queue SIGCHLD for parent process */
                signal_queue(LINUX_SIGCHLD);
                return gpid;
            } else if (ret == 0) {
                return 0; /* WNOHANG */
            }
            return linux_errno();
        }
    }

    return -LINUX_ECHILD;
}

/* ---------- sys_waitid ---------- */

/* Linux siginfo_t field offsets on aarch64 (LP64) */
#define SIGINFO_SIZE         128
#define SIGINFO_OFF_SIGNO      0   /* int32_t si_signo */
#define SIGINFO_OFF_ERRNO      4   /* int32_t si_errno */
#define SIGINFO_OFF_CODE       8   /* int32_t si_code */
#define SIGINFO_OFF_PID       16   /* pid_t (int32_t) */
#define SIGINFO_OFF_UID       20   /* uid_t (uint32_t) */
#define SIGINFO_OFF_STATUS    24   /* int32_t si_status */

/* si_code values for SIGCHLD */
#define CLD_EXITED   1
#define CLD_KILLED   2
#define CLD_DUMPED   3

/* waitid idtype values */
#define P_ALL  0
#define P_PID  1
#define P_PGID 2

int64_t sys_waitid(guest_t *g, int idtype, int64_t id,
                   uint64_t infop_gva, int options) {
    /* Translate options: Linux WEXITED=4, WNOHANG=1, WSTOPPED=2 */
    int mac_options = 0;
    if (options & 1) mac_options |= WNOHANG;
    if (options & 2) mac_options |= WUNTRACED;
    /* WEXITED (4) is implied by waitpid */

    /* Convert idtype+id to a waitpid-compatible pid argument */
    pid_t wait_pid;
    switch (idtype) {
    case P_ALL:  wait_pid = -1; break;
    case P_PID:  wait_pid = (pid_t)id; break;
    case P_PGID: wait_pid = -(pid_t)id; break;
    default:     return -LINUX_EINVAL;
    }

    /* Search process table for matching entry */
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (!proc_table[i].active) continue;

        /* Match: P_ALL matches any, P_PID matches guest_pid */
        if (idtype == P_PID && proc_table[i].guest_pid != wait_pid)
            continue;

        int status;
        pid_t ret;

        if (proc_table[i].exited) {
            /* Already reaped (from CLONE_VFORK wait) */
            status = proc_table[i].exit_status;
            ret = proc_table[i].host_pid;
        } else {
            ret = waitpid(proc_table[i].host_pid, &status, mac_options);
            if (ret == 0) return 0; /* WNOHANG, child not yet exited */
            if (ret < 0) return linux_errno();
        }

        /* Fill siginfo_t in guest memory */
        if (infop_gva) {
            uint8_t si[SIGINFO_SIZE];
            memset(si, 0, sizeof(si));

            int32_t signo = LINUX_SIGCHLD;
            int32_t si_code;
            int32_t si_status;

            if (WIFEXITED(status)) {
                si_code = CLD_EXITED;
                si_status = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                si_code = WCOREDUMP(status) ? CLD_DUMPED : CLD_KILLED;
                si_status = WTERMSIG(status);
            } else {
                si_code = CLD_EXITED;
                si_status = 0;
            }

            memcpy(si + SIGINFO_OFF_SIGNO, &signo, 4);
            memcpy(si + SIGINFO_OFF_CODE, &si_code, 4);
            int32_t gpid32 = (int32_t)proc_table[i].guest_pid;
            memcpy(si + SIGINFO_OFF_PID, &gpid32, 4);
            uint32_t uid = 0;
            memcpy(si + SIGINFO_OFF_UID, &uid, 4);
            memcpy(si + SIGINFO_OFF_STATUS, &si_status, 4);

            guest_write(g, infop_gva, si, SIGINFO_SIZE);
        }

        /* Don't remove from table if WNOWAIT is set */
        if (!(options & 0x01000000))
            proc_table[i].active = 0;

        return 0; /* waitid returns 0 on success */
    }

    return -LINUX_ECHILD;
}

/* ---------- vCPU run loop ---------- */

/* Global vCPU handle for the SIGALRM handler (unavoidable global state —
 * signal handlers cannot receive context parameters). */
static hv_vcpu_t g_timeout_vcpu;
static volatile sig_atomic_t g_timed_out;

static void alarm_handler(int sig) {
    (void)sig;
    g_timed_out = 1;
    hv_vcpus_exit(&g_timeout_vcpu, 1);
}

int vcpu_run_loop(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit,
                  guest_t *g, int verbose, int timeout_sec) {
    int exit_code = 0;
    int running = 1;
    int iter = 0;

    /* Set up timeout handling */
    g_timeout_vcpu = vcpu;
    g_timed_out = 0;
    signal(SIGALRM, alarm_handler);

    while (running) {
        if (verbose) {
            uint64_t pc;
            hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
            fprintf(stderr, "hl: [%d] vcpu_run PC=0x%llx\n",
                    iter, (unsigned long long)pc);
        }
        iter++;

        /* Arm per-iteration timeout (hl's own safety timeout).
         * Guest ITIMER_REAL is emulated internally by signal_check_timer()
         * rather than using host setitimer, because macOS shares alarm()
         * and setitimer(ITIMER_REAL) as the same underlying timer. */
        alarm((unsigned)timeout_sec);

        HV_CHECK(hv_vcpu_run(vcpu));

        /* Disarm timeout */
        alarm(0);

        /* Check for timeout */
        if (g_timed_out) {
            fprintf(stderr, "hl: vCPU execution timed out after %ds\n",
                    timeout_sec);

            uint64_t pc, cpsr;
            hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
            hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
            fprintf(stderr, "hl: timeout state: PC=0x%llx CPSR=0x%llx\n",
                    (unsigned long long)pc, (unsigned long long)cpsr);

            uint64_t esr, far_reg, elr, sctlr_val;
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &esr);
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &far_reg);
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &elr);
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr_val);
            fprintf(stderr, "hl: ESR_EL1=0x%llx FAR_EL1=0x%llx "
                    "ELR_EL1=0x%llx SCTLR_EL1=0x%llx\n",
                    (unsigned long long)esr,
                    (unsigned long long)far_reg,
                    (unsigned long long)elr,
                    (unsigned long long)sctlr_val);

            exit_code = 124;
            break;
        }

        if (vexit->reason == HV_EXIT_REASON_EXCEPTION) {
            uint32_t ec = (vexit->exception.syndrome >> 26) & 0x3F;

            if (ec == 0x16) {
                /* HVC exit */
                uint16_t imm = vexit->exception.syndrome & 0xFFFF;

                if (verbose)
                    fprintf(stderr, "hl: HVC #%u\n", imm);

                switch (imm) {
                case 5: {
                    /* HVC #5: Linux syscall forwarding */
                    int ret = syscall_dispatch(vcpu, g, &exit_code, verbose);
                    if (ret == 1)
                        running = 0;
                    /* ret == SYSCALL_EXEC_HAPPENED: exec replaced the process,
                     * X0 already set by sys_execve, just continue loop */

                    /* Check guest ITIMER_REAL expiry (queues SIGALRM if due) */
                    signal_check_timer();

                    /* Deliver pending signals after each syscall */
                    if (running && signal_pending()) {
                        int sig_ret = signal_deliver(vcpu, g, &exit_code);
                        if (sig_ret < 0)
                            running = 0; /* Default TERM/CORE disposition */
                        /* sig_ret == 1: signal delivered to handler, continue */
                    }
                    break;
                }

                case 0: {
                    /* HVC #0: Normal exit */
                    uint64_t x0;
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
                    if (verbose)
                        fprintf(stderr, "hl: guest exit HVC #0 code=%llu\n",
                                (unsigned long long)x0);
                    exit_code = (int)x0;
                    running = 0;
                    break;
                }

                case 4: {
                    /* HVC #4: Set system register (from shim) */
                    uint64_t reg_id, value;
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &reg_id);
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &value);

                    hv_sys_reg_t hv_reg;
                    switch ((int)reg_id) {
                    case 0: hv_reg = HV_SYS_REG_VBAR_EL1; break;
                    case 1: hv_reg = HV_SYS_REG_MAIR_EL1; break;
                    case 2: hv_reg = HV_SYS_REG_TCR_EL1;  break;
                    case 3: hv_reg = HV_SYS_REG_TTBR0_EL1; break;
                    case 4: hv_reg = HV_SYS_REG_SCTLR_EL1; break;
                    case 5: hv_reg = HV_SYS_REG_CPACR_EL1; break;
                    case 6: hv_reg = HV_SYS_REG_ELR_EL1; break;
                    case 7: hv_reg = HV_SYS_REG_SPSR_EL1; break;
                    case 8: hv_reg = HV_SYS_REG_TTBR1_EL1; break;
                    default:
                        fprintf(stderr, "hl: HVC #4 unknown reg %llu\n",
                                (unsigned long long)reg_id);
                        exit_code = 128;
                        running = 0;
                        continue;
                    }
                    if (verbose)
                        fprintf(stderr, "hl: HVC #4 set reg %llu = 0x%llx\n",
                                (unsigned long long)reg_id,
                                (unsigned long long)value);
                    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, hv_reg, value));
                    break;
                }

                case 2: {
                    /* HVC #2: Bad exception in guest */
                    uint64_t x0, x1, x2, x3, x5;
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1);
                    hv_vcpu_get_reg(vcpu, HV_REG_X2, &x2);
                    hv_vcpu_get_reg(vcpu, HV_REG_X3, &x3);
                    hv_vcpu_get_reg(vcpu, HV_REG_X5, &x5);
                    fprintf(stderr, "hl: guest exception vec=0x%03llx "
                            "ESR=0x%llx FAR=0x%llx ELR=0x%llx SPSR=0x%llx\n",
                            (unsigned long long)x5,
                            (unsigned long long)x0,
                            (unsigned long long)x1,
                            (unsigned long long)x2,
                            (unsigned long long)x3);
                    exit_code = 128;
                    running = 0;
                    break;
                }

                default:
                    fprintf(stderr, "hl: unexpected HVC #%u\n", imm);
                    exit_code = 128;
                    running = 0;
                    break;
                }
            } else if (ec == 0x01) {
                /* WFI/WFE trapped — just continue */
                if (verbose)
                    fprintf(stderr, "hl: WFI/WFE trapped\n");
            } else {
                /* Non-HVC exception at EL2 level */
                fprintf(stderr, "hl: unexpected exception EC=0x%x "
                        "syndrome=0x%llx VA=0x%llx PA=0x%llx\n",
                        ec,
                        (unsigned long long)vexit->exception.syndrome,
                        (unsigned long long)vexit->exception.virtual_address,
                        (unsigned long long)vexit->exception.physical_address);
                exit_code = 128;
                running = 0;
            }
        } else if (vexit->reason == HV_EXIT_REASON_CANCELED) {
            if (!g_timed_out) {
                fprintf(stderr, "hl: vCPU canceled\n");
                exit_code = 128;
                running = 0;
            }
        } else {
            fprintf(stderr, "hl: unexpected exit reason 0x%x\n",
                    vexit->reason);
            exit_code = 128;
            running = 0;
        }
    }

    /* Clean up timeout */
    signal(SIGALRM, SIG_DFL);
    alarm(0);

    return exit_code;
}
