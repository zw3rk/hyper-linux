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
#include <sys/wait.h>
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

    /* Signal TLB invalidation needed */
    g->need_tlbi = 1;

    if (verbose) {
        fprintf(stderr, "hl: execve: loaded %s, entry=0x%llx sp=0x%llx\n",
                path, (unsigned long long)entry_ipa, (unsigned long long)sp_ipa);
    }

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

    /* Restore system registers */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, regs.vbar_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, regs.mair_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, regs.tcr_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, regs.ttbr0_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, regs.sctlr_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, regs.cpacr_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, regs.elr_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, regs.spsr_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, regs.sp_el0));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL1, regs.sp_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, regs.tpidr_el0));

    /* The child process is already in EL1 (svc_handler context), about to
     * return from the clone syscall. Set PC to the shim's ERET point.
     * The shim restores registers from the EL1 stack and ERETs to EL0.
     *
     * However, since we're starting from a fresh vCPU, we need to enter
     * through the shim entry point to enable the MMU first. The shim
     * will ERET to ELR_EL1 which points back to the guest's clone
     * return point. */
    uint64_t shim_ipa = guest_ipa(&g, SHIM_BASE);
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, shim_ipa));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5)); /* EL1h */

    /* Zero all GPRs first */
    for (int i = 0; i < 31; i++)
        HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, 0));

    /* X0 = SCTLR value with MMU enable (shim reads this for HVC #4) */
    uint64_t sctlr_with_mmu = SCTLR_RES1 | SCTLR_M | SCTLR_C | SCTLR_I;
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, sctlr_with_mmu));

    /* The ELR_EL1 is set to the clone return point. After the shim enables
     * the MMU and ERETs, the guest resumes at that point with X0=0 (child). */

    /* Set the child's return value: X0=0.
     * ELR_EL1 already points to the return address. We need SP_EL0 to be
     * the guest's SP. The shim will ERET to ELR_EL1 with X0=0. But the
     * shim entry zeroes X0 before ERET. We need ELR_EL1 to point to the
     * clone return site, and the guest X0 must be 0.
     * Since the shim zeros all regs and ERETs, the guest will get X0=0
     * automatically. This is exactly what we want for the child. */

    proc_init();
    proc_set_shim(NULL, 0); /* Child doesn't need shim blob for later execs yet */

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
                return gpid;
            } else if (ret == 0) {
                return 0; /* WNOHANG */
            }
            return linux_errno();
        }
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

        /* Arm per-iteration timeout */
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
