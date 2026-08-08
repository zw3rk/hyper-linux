/* fork_ipc.c — Fork/clone IPC for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements clone via posix_spawn + IPC state transfer. macOS HVF allows
 * only one VM per process, so fork spawns a new hl process and serializes
 * the full VM state (registers, memory, FDs) over a socketpair.
 */
#include "fork_ipc.h"
#include "device.h"   /* hl_device_node_index/by_index */
#include "syscall_proc.h"    /* proc_get_pid, proc_set_identity, proc_alloc_pid, proc_register_child, proc_mark_child_exited, proc_set_shim, proc_get_shim_blob, proc_get_shim_size, proc_init */
#include "syscall_internal.h" /* fd_table, FD_TABLE_SIZE */
#include "syscall.h"         /* syscall_init, FD_CLOSED, FD_STDIO, etc. */
#include "syscall_signal.h"  /* signal_get_state, signal_set_state, signal_state_t */
#include "syscall_poll.h"    /* wakeup_pipe_signal */
#include "hv_util.h"         /* HV_CHECK, SCTLR_* constants */
#include "guest.h"           /* guest_t, guest_init, guest_destroy, guest_get_used_regions, etc. */
#include "thread.h"          /* thread_alloc, thread_alloc_sp_el1, current_thread */
#include "vdso.h"            /* vdso_build, vdso_publisher_start/stop */
#include "syscall_shm.h"     /* hl_shm_fork_export/import */
#include "futex.h"           /* futex_wake_one */
#include "fd_object.h"
#include "audio_oss.h"
#include "audio.h"           /* hl_audio_backend_name */
#include "vfs.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/spawn.h>          /* POSIX_SPAWN_CLOEXEC_DEFAULT (macOS extension) */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <dirent.h>             /* fdopendir, for DIR* reconstruction in child */
#include <sys/wait.h>
#include <mach-o/dyld.h>

/* ---------- IPC Protocol for fork ---------- */

/* Magic values for IPC frame delimiters */
#define IPC_MAGIC_HEADER  0x484C464BU  /* "HLFK" */
#define IPC_MAGIC_SENTINEL 0x484C4F4BU /* "HLOK" */
#define IPC_VERSION       6            /* v6: + SysV SHM segment records */
#define IPC_FD_HAS_OBJECT 1            /* ipc_fd_entry_t.pad: synthetic open-file follows */

/* IPC header: sent first over socketpair */
typedef struct {
    uint32_t magic;
    uint32_t version;        /* Protocol version for forward compatibility */
    uint32_t ipa_bits;       /* IPA width for HVF VM (e.g., 36, 40, 48) */
    uint32_t has_shm;        /* Non-zero: shm fd follows (COW fork path) */
    int64_t  child_pid;
    int64_t  parent_pid;
    /* Guest state */
    uint64_t guest_size;     /* IPA-derived address space size (child must match) */
    uint64_t brk_base;
    uint64_t brk_current;
    uint64_t stack_base;     /* Dynamic stack position (v4+) */
    uint64_t stack_top;      /* Dynamic stack top */
    uint64_t mmap_next;
    uint64_t mmap_end;
    uint64_t pt_pool_next;
    uint64_t ttbr0;
    /* Rosetta state (v3+) */
    uint32_t is_rosetta;     /* Non-zero when running x86_64 via rosetta */
    uint32_t pad;
    uint64_t ttbr1;          /* TTBR1 value for kernel VA page tables */
    uint64_t kbuf_gpa;       /* GPA of kernel VA buffer in primary region */
    uint64_t mmap_rx_next;   /* RX mmap high-water mark */
    uint64_t mmap_rx_end;    /* Current RX mmap limit */
    /* Rosetta placement (survives guest_reset for execve re-setup) */
    uint64_t rosetta_guest_base; /* GPA in primary buffer where rosetta is loaded */
    uint64_t rosetta_va_base;    /* High VA start (e.g. 0x800000000000) */
    uint64_t rosetta_size;       /* 2MB-aligned total rosetta span */
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
    uint64_t ttbr1_el1;  /* Kernel VA page tables (rosetta mode) */
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
        if (cmsg->cmsg_len < CMSG_LEN(0)) {
            free(cmsg_buf);
            return -1;  /* malformed cmsg */
        }
        int n = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        if (n > max_count) n = max_count;
        memcpy(fds, CMSG_DATA(cmsg), n * sizeof(int));
        *out_count = n;
    }

    free(cmsg_buf);
    return 0;
}

/* ---------- fork_child_main ---------- */

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
    if (hdr.version != IPC_VERSION) {
        fprintf(stderr, "hl: fork-child: IPC version mismatch "
                "(got %u, expected %u)\n", hdr.version, IPC_VERSION);
        return 1;
    }

    if (verbose)
        fprintf(stderr, "hl: fork-child: pid=%lld ppid=%lld\n",
                (long long)hdr.child_pid, (long long)hdr.parent_pid);

    /* Set process identity via accessor (static state lives in syscall_proc.c) */
    proc_set_identity(hdr.child_pid, hdr.parent_pid);

    /* Step 2: Create guest VM — COW path (shm) or legacy path (region copy) */
    guest_t g;

    if (hdr.has_shm) {
        /* COW fork: receive shm fd via SCM_RIGHTS, then map MAP_PRIVATE.
         * This gives us an instant copy-on-write snapshot of the parent's
         * entire guest memory — no region enumeration or byte copying. */
        int shm_fd = -1;
        int shm_count = 0;
        if (recv_fds(ipc_fd, &shm_fd, 1, &shm_count) < 0 || shm_count != 1) {
            fprintf(stderr, "hl: fork-child: failed to receive shm fd\n");
            close(ipc_fd);
            return 1;
        }
        if (guest_init_from_shm(&g, shm_fd, hdr.guest_size, hdr.ipa_bits) < 0) {
            fprintf(stderr, "hl: fork-child: guest_init_from_shm failed\n");
            close(ipc_fd);
            return 1;
        }
        if (verbose)
            fprintf(stderr, "hl: fork-child: COW fork via shm fd\n");
    } else {
        /* Legacy path: allocate fresh guest memory and receive regions */
        if (guest_init(&g, hdr.guest_size, hdr.ipa_bits, (int)hdr.is_rosetta) < 0) {
            fprintf(stderr, "hl: fork-child: failed to init guest\n");
            close(ipc_fd);
            return 1;
        }
    }

    /* Restore guest allocation state */
    g.brk_base = hdr.brk_base;
    g.brk_current = hdr.brk_current;
    g.stack_base = hdr.stack_base;
    g.stack_top = hdr.stack_top;
    g.mmap_next = hdr.mmap_next;
    g.mmap_end = hdr.mmap_end;
    g.pt_pool_next = hdr.pt_pool_next;
    g.ttbr0 = hdr.ttbr0;

    /* Restore rosetta state */
    g.is_rosetta = (int)hdr.is_rosetta;
    g.ttbr1 = hdr.ttbr1;
    g.kbuf_gpa = hdr.kbuf_gpa;
    g.mmap_rx_next = hdr.mmap_rx_next;
    g.mmap_rx_end = hdr.mmap_rx_end;
    g.rosetta_guest_base = hdr.rosetta_guest_base;
    g.rosetta_va_base    = hdr.rosetta_va_base;
    g.rosetta_size       = hdr.rosetta_size;
    g.verbose            = verbose;
    if (g.kbuf_gpa != 0)
        g.kbuf_base = (uint8_t *)g.host_base + g.kbuf_gpa;

    /* Step 3: Read registers */
    ipc_registers_t regs;
    if (ipc_read_all(ipc_fd, &regs, sizeof(regs)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read registers\n");
        guest_destroy(&g);
        return 1;
    }

    /* Step 4: Read memory regions (0 regions in COW path) */
    uint32_t num_regions;
    if (ipc_read_all(ipc_fd, &num_regions, sizeof(num_regions)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read region count\n");
        guest_destroy(&g);
        return 1;
    }

    if (verbose && num_regions > 0)
        fprintf(stderr, "hl: fork-child: receiving %u memory regions\n",
                num_regions);

    for (uint32_t i = 0; i < num_regions; i++) {
        ipc_region_header_t rhdr;
        if (ipc_read_all(ipc_fd, &rhdr, sizeof(rhdr)) < 0) {
            fprintf(stderr, "hl: fork-child: failed to read region header\n");
            guest_destroy(&g);
            return 1;
        }

        if (rhdr.offset > g.guest_size ||
            rhdr.size > g.guest_size - rhdr.offset) {
            fprintf(stderr, "hl: fork-child: region out of bounds\n");
            guest_destroy(&g);
            return 1;
        }

        /* Read region data in chunks */
        uint8_t *dst = (uint8_t *)g.host_base + rhdr.offset;
        size_t remaining = rhdr.size;
        while (remaining > 0) {
            size_t chunk = remaining > ((size_t)1024 * 1024) ? ((size_t)1024 * 1024) : remaining;
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

    /* Initialize our FD table and reset mmap gap-finder hints
     * (parent's gap hints are stale for the child's allocator). */
    syscall_init();
    mmap_reset_hints();

    /* Validate num_fds to prevent integer overflow in multiplication
     * and allocation of unreasonably large buffers from malformed IPC. */
    if (num_fds > FD_TABLE_SIZE) {
        fprintf(stderr, "hl: fork-child: num_fds %u exceeds FD_TABLE_SIZE\n",
                num_fds);
        guest_destroy(&g);
        return 1;
    }

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

        if (received_count != (int)num_fds) {
            fprintf(stderr, "hl: fork-child: fd count mismatch: "
                    "received %d, expected %u\n",
                    received_count, num_fds);
            for (int fi = 0; fi < received_count; fi++)
                close(host_fds[fi]);
            free(host_fds);
            free(fd_entries);
            guest_destroy(&g);
            return 1;
        }

        /* Populate fd_table.  Parent sends fd_entries and host_fds in
         * 1:1 correspondence — each entry at index i has its host FD
         * at host_fds[i] (STDIO entries include their FD too, but we
         * use the child's existing stdio instead).
         * pad & IPC_FD_HAS_OBJECT: host_fd is a placeholder; real open-file
         * is recreated from object records after SCM_RIGHTS (IPC v5). */
        for (uint32_t i = 0; i < num_fds; i++) {
            int gfd = fd_entries[i].guest_fd;
            if (gfd < 0 || gfd >= FD_TABLE_SIZE) continue;

            if (fd_entries[i].type == FD_STDIO) {
                /* stdio fds are already set up by syscall_init.
                 * Close the received fd — SCM_RIGHTS created a new fd
                 * in the child, but we use the child's own stdio. */
                if ((int)i < received_count)
                    close(host_fds[i]);
                fd_table[gfd].linux_flags = fd_entries[i].linux_flags;
            } else if ((int)i < received_count) {
                if (fd_entries[i].pad & IPC_FD_HAS_OBJECT) {
                    /* Placeholder only — close; object import fills slot. */
                    close(host_fds[i]);
                    /* Temporarily mark slot used so bitmap stays consistent;
                     * import will fd_alloc_at again. */
                    fd_alloc_at(gfd, fd_entries[i].type, -1);
                    fd_table[gfd].linux_flags = fd_entries[i].linux_flags;
                } else {
                    /* Use fd_alloc_at to properly update the bitmap.
                     * Without this, fd_alloc() would see these slots as
                     * free and overwrite them on the first allocation. */
                    fd_alloc_at(gfd, fd_entries[i].type, host_fds[i]);
                    fd_table[gfd].linux_flags = fd_entries[i].linux_flags;

                    /* Re-derive FD_DEVICE's registry pointer from the index
                     * carried in pad, so fstat keeps the Linux numbers (V6). */
                    if (fd_entries[i].type == FD_DEVICE)
                        fd_table[gfd].dir =
                            (void *)hl_device_node_by_index(fd_entries[i].pad);

                    /* Reconstruct DIR* for directory FDs. The parent's DIR*
                     * pointer is meaningless in the child's address space.
                     * fdopendir() takes ownership of the fd, so dup() first
                     * to keep the original host_fd usable for other ops. */
                    if (fd_entries[i].type == FD_DIR) {
                        int dir_fd = dup(host_fds[i]);
                        if (dir_fd >= 0) {
                            DIR *dir = fdopendir(dir_fd);
                            if (dir) {
                                fd_table[gfd].dir = dir;
                            } else {
                                close(dir_fd);
                                fprintf(stderr, "hl: fork-child: fdopendir "
                                        "failed for gfd %d\n", gfd);
                            }
                        } else {
                            fprintf(stderr, "hl: fork-child: dup failed for "
                                    "DIR gfd %d: %s\n", gfd, strerror(errno));
                        }
                    }
                }
            }
        }

        free(host_fds);
        free(fd_entries);
    }

    /* IPC v5: open-file object records (OSS recreate-empty). Always present. */
    {
        uint32_t num_objects = 0;
        if (ipc_read_all(ipc_fd, &num_objects, sizeof(num_objects)) < 0) {
            fprintf(stderr, "hl: fork-child: failed to read object count\n");
            guest_destroy(&g);
            return 1;
        }
        for (uint32_t oi = 0; oi < num_objects; oi++) {
            int32_t gfd = -1;
            hl_fork_object_record_t rec;
            if (ipc_read_all(ipc_fd, &gfd, sizeof(gfd)) < 0 ||
                ipc_read_all(ipc_fd, &rec, sizeof(rec)) < 0) {
                fprintf(stderr, "hl: fork-child: failed to read object %u\n", oi);
                guest_destroy(&g);
                return 1;
            }
            if (gfd < 0 || gfd >= FD_TABLE_SIZE) continue;

            hl_open_file_t *of = NULL;
            if (hl_oss_fd_needs_recreate((int)rec.kind))
                of = hl_oss_fork_import(&rec);
            if (!of) {
                fprintf(stderr, "hl: fork-child: fork_import failed gfd=%d kind=%u\n",
                        gfd, rec.kind);
                continue;
            }
            int host_alias = -1;
            if (of->ops && of->ops->poll_host_fd) {
                int pfd = of->ops->poll_host_fd(of, -1);
                if (pfd >= 0) host_alias = dup(pfd);
            }

            uint32_t fdflags = 0;
            if (fd_table[gfd].linux_flags & LINUX_O_CLOEXEC)
                fdflags = LINUX_O_CLOEXEC;
            hl_descriptor_t *d = hl_descriptor_create(of, host_alias, fdflags,
                                                      of->kind, NULL);
            if (!d) {
                hl_open_file_release(of);
                if (host_alias >= 0) close(host_alias);
                continue;
            }
            pthread_mutex_lock(&fd_lock);
            fd_table[gfd].desc = d;
            fd_table[gfd].of = of;
            fd_table[gfd].type = of->kind;
            fd_table[gfd].host_fd = host_alias;
            fd_table[gfd].linux_flags = (int)(fdflags |
                (int)atomic_load(&of->status_flags));
            pthread_mutex_unlock(&fd_lock);

            if (hl_trace_on(HL_TRACE_FORK))
                hl_trace(HL_TRACE_FORK,
                         "import gfd=%d kind=%d policy=recreate-empty of=%llu",
                         gfd, of->kind, (unsigned long long)of->object_id);
        }
    }

    /* Step 6: Read process info (cwd + umask) */
    char cwd[LINUX_PATH_MAX];
    uint32_t umask_val;
    if (ipc_read_all(ipc_fd, cwd, sizeof(cwd)) < 0 ||
        ipc_read_all(ipc_fd, &umask_val, sizeof(umask_val)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read process info\n");
        guest_destroy(&g);
        return 1;
    }

    /* VFS config reconstruction */
    {
        hl_vfs_t vfs_snap;
        if (ipc_read_all(ipc_fd, &vfs_snap, sizeof(vfs_snap)) < 0) {
            fprintf(stderr, "hl: fork-child: failed to read VFS config\n");
            guest_destroy(&g);
            return 1;
        }
        hl_vfs_fork_import(&vfs_snap);
    }

    /* SysV SHM segments: re-attach the same shmids and restore the Stage-2
     * override so the inherited guest page tables resolve to shared memory. */
    {
        int32_t nshm = 0;
        if (ipc_read_all(ipc_fd, &nshm, sizeof(nshm)) < 0) {
            fprintf(stderr, "hl: fork-child: failed to read SHM count\n");
            guest_destroy(&g);
            return 1;
        }
        if (nshm < 0 || nshm > hl_shm_fork_max()) {
            fprintf(stderr, "hl: fork-child: bad SHM count %d\n", nshm);
            guest_destroy(&g);
            return 1;
        }
        if (nshm > 0) {
            hl_shm_fork_rec_t *recs =
                calloc((size_t)nshm, sizeof(*recs));
            if (!recs) { guest_destroy(&g); return 1; }
            if (ipc_read_all(ipc_fd, recs,
                             (size_t)nshm * sizeof(*recs)) < 0) {
                fprintf(stderr, "hl: fork-child: failed to read SHM recs\n");
                free(recs);
                guest_destroy(&g);
                return 1;
            }
            hl_shm_fork_import(&g, recs, nshm);
            free(recs);
        }
    }

    if (hl_vfs_mode() == HL_FS_ROOTED) {
        if (cwd[0] != '\0')
            hl_vfs_set_cwd(cwd);
        /* never host chdir in rooted mode */
    } else if (cwd[0] != '\0') {
        chdir(cwd);
    }
    umask((mode_t)umask_val);

    /* Step 6a: Read sysroot path */
    char sysroot_ipc[LINUX_PATH_MAX];
    if (ipc_read_all(ipc_fd, sysroot_ipc, sizeof(sysroot_ipc)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read sysroot\n");
        guest_destroy(&g);
        return 1;
    }
    if (sysroot_ipc[0] != '\0')
        proc_set_sysroot(sysroot_ipc);

    /* Step 6a2: Read ELF path (/proc/self/exe) and hl path (rosettad) */
    char elf_path_ipc[LINUX_PATH_MAX];
    if (ipc_read_all(ipc_fd, elf_path_ipc, sizeof(elf_path_ipc)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read elf path\n");
        guest_destroy(&g);
        return 1;
    }
    if (elf_path_ipc[0] != '\0')
        proc_set_elf_path(elf_path_ipc);

    char hl_path_ipc[LINUX_PATH_MAX];
    if (ipc_read_all(ipc_fd, hl_path_ipc, sizeof(hl_path_ipc)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read hl path\n");
        guest_destroy(&g);
        return 1;
    }
    if (hl_path_ipc[0] != '\0')
        proc_set_hl_path(hl_path_ipc);

    /* Step 6a3: Read cmdline (/proc/self/cmdline) */
    uint32_t cmdline_len_u32;
    if (ipc_read_all(ipc_fd, &cmdline_len_u32, sizeof(cmdline_len_u32)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read cmdline len\n");
        guest_destroy(&g);
        return 1;
    }
    if (cmdline_len_u32 > 0) {
        /* Always drain the cmdline bytes to keep the IPC stream in sync.
         * If the cmdline is within a reasonable size, parse and use it. */
        char *cmdline_buf = (cmdline_len_u32 <= LINUX_PATH_MAX * 4)
                          ? malloc(cmdline_len_u32) : NULL;
        if (cmdline_buf) {
            if (ipc_read_all(ipc_fd, cmdline_buf, cmdline_len_u32) < 0) {
                free(cmdline_buf);
                fprintf(stderr, "hl: fork-child: failed to read cmdline\n");
                guest_destroy(&g);
                return 1;
            }
            proc_set_cmdline_raw(cmdline_buf, cmdline_len_u32);
            free(cmdline_buf);
        } else {
            /* Drain cmdline bytes to keep the IPC stream synchronized —
             * subsequent reads expect region data next. */
            if (cmdline_len_u32 > LINUX_PATH_MAX * 4)
                fprintf(stderr, "hl: fork-child: cmdline too large (%u), skipping\n",
                        cmdline_len_u32);
            else
                fprintf(stderr, "hl: fork-child: cmdline malloc failed\n");
            char drain[256];
            uint32_t remaining = cmdline_len_u32;
            while (remaining > 0) {
                uint32_t chunk = remaining < sizeof(drain) ? remaining : sizeof(drain);
                if (ipc_read_all(ipc_fd, drain, chunk) < 0) {
                    guest_destroy(&g);
                    return 1;
                }
                remaining -= chunk;
            }
        }
    }

    /* Step 6b: Read semantic region tracking */
    uint32_t num_guest_regions;
    if (ipc_read_all(ipc_fd, &num_guest_regions, sizeof(num_guest_regions)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read region count\n");
        guest_destroy(&g);
        return 1;
    }
    if (num_guest_regions > GUEST_MAX_REGIONS)
        num_guest_regions = GUEST_MAX_REGIONS;
    if (num_guest_regions > 0) {
        if (ipc_read_all(ipc_fd, g.regions,
                         num_guest_regions * sizeof(guest_region_t)) < 0) {
            fprintf(stderr, "hl: fork-child: failed to read regions\n");
            guest_destroy(&g);
            return 1;
        }
    }
    g.nregions = (int)num_guest_regions;

    /* Step 6b2: Read VA aliases (for rosetta high-VA mappings) */
    uint32_t num_aliases;
    if (ipc_read_all(ipc_fd, &num_aliases, sizeof(num_aliases)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read alias count\n");
        guest_destroy(&g);
        return 1;
    }
    if (num_aliases > GUEST_MAX_ALIASES) {
        fprintf(stderr, "hl: fork-child: too many VA aliases: %u (max %d)\n",
                num_aliases, GUEST_MAX_ALIASES);
        guest_destroy(&g);
        return 1;
    }
    if (num_aliases > 0) {
        if (ipc_read_all(ipc_fd, g.va_aliases,
                         num_aliases * sizeof(guest_va_alias_t)) < 0) {
            fprintf(stderr, "hl: fork-child: failed to read VA aliases\n");
            guest_destroy(&g);
            return 1;
        }
    }
    g.naliases = (int)num_aliases;

    /* Step 6b3: Read preannounced regions (rosetta /proc/self/maps) */
    uint32_t num_preannounced;
    if (ipc_read_all(ipc_fd, &num_preannounced, sizeof(num_preannounced)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read preannounced count\n");
        guest_destroy(&g);
        return 1;
    }
    if (num_preannounced > GUEST_MAX_PREANNOUNCED) {
        fprintf(stderr, "hl: fork-child: too many preannounced regions: "
                "%u (max %d)\n", num_preannounced, GUEST_MAX_PREANNOUNCED);
        guest_destroy(&g);
        return 1;
    }
    if (num_preannounced > 0) {
        if (ipc_read_all(ipc_fd, g.preannounced,
                         num_preannounced * sizeof(guest_region_t)) < 0) {
            fprintf(stderr, "hl: fork-child: failed to read preannounced\n");
            guest_destroy(&g);
            return 1;
        }
    }
    g.npreannounced = (int)num_preannounced;

    /* Step 6c: Read signal state (shifted to accommodate region tracking) */
    signal_state_t sig;
    if (ipc_read_all(ipc_fd, &sig, sizeof(sig)) < 0) {
        fprintf(stderr, "hl: fork-child: failed to read signal state\n");
        guest_destroy(&g);
        return 1;
    }
    /* POSIX: "Signals pending to the parent shall not be pending to the
     * child."  Clear pending bitmask and RT queue before applying state.
     * Note: signal_set_state() is deferred until after thread_register_main()
     * so that current_thread is non-NULL and per-thread state (blocked mask,
     * altstack) is properly restored. See signal_set_state() line 253. */
    sig.pending = 0;
    memset(sig.rt_queue, 0, sizeof(sig.rt_queue));

    /* Step 6d: Read shim blob (needed for exec in child) */
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
        /* Note: shim memory is leaked intentionally -- it must outlive the
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

    /* Restore TTBR1 for rosetta kernel VA mappings. Without this, fork
     * children in rosetta mode can't access kbuf at kernel VA. */
    if (regs.ttbr1_el1 != 0)
        HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR1_EL1, regs.ttbr1_el1));

    /* Enable MMU directly (page tables already in guest memory from IPC).
     * SCTLR must include MMU-enable (M), caches (C, I), RES1 bits,
     * and EL0 cache maintenance access (UCI, UCT) for JIT translators. */
    uint64_t sctlr_with_mmu = SCTLR_RES1 | SCTLR_M | SCTLR_C | SCTLR_I
                             | SCTLR_DZE | SCTLR_UCT | SCTLR_UCI;
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
    /* Restore identity after proc_init (which resets pid/ppid to 1/0) */
    proc_set_identity(hdr.child_pid, hdr.parent_pid);
    /* proc_set_shim was called above from IPC data (step 6d) */

    /* Register the fork child's main thread in the thread table.
     * Without this, current_thread is NULL and any syscall handler that
     * accesses per-thread state (signal masks, ptrace, CLONE_THREAD)
     * will dereference NULL. */
    thread_register_main(vcpu, vexit, hdr.child_pid, regs.sp_el1);

    /* Now that current_thread is set, apply signal state. This must happen
     * after thread_register_main() so the per-thread blocked mask and
     * altstack are properly restored to the thread entry. */
    signal_set_state(&sig);

    /* The [vvar] time page is written only by the vDSO publisher thread,
     * which does not exist in a fork child (threads are not inherited). The
     * COW child maps the backing file MAP_PRIVATE, so it never sees the
     * parent's updates either — its clock_gettime()/gettimeofday() would be
     * frozen at the fork instant forever, and the guest takes the vDSO fast
     * path because the inherited page still carries a valid version stamp.
     * Re-establish the page and restart the publisher here. vdso_build()
     * also covers the legacy IPC copy path, which never transferred the
     * [vdso]/[vvar] pages at all. */
    vdso_build(&g);
    vdso_publisher_start(&g);

    if (verbose)
        fprintf(stderr, "hl: fork-child: entering vCPU loop\n");

    /* Step 9: Enter vCPU run loop */
    int exit_code = vcpu_run_loop(vcpu, vexit, &g, verbose, timeout_sec, 1 /* forked main */);

    vdso_publisher_stop();
    guest_destroy(&g);
    return exit_code;
}

/* ---------- sys_clone ---------- */

/* Linux clone flags */
#define LINUX_CLONE_VM             0x00000100
#define LINUX_CLONE_VFORK          0x00004000
#define LINUX_CLONE_THREAD         0x00010000
#define LINUX_CLONE_SETTLS         0x00080000
#define LINUX_CLONE_PARENT_SETTID  0x00100000
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000
#define LINUX_CLONE_CHILD_SETTID   0x01000000
/* LINUX_SIGCHLD defined in syscall_signal.h (included above) */

/* ---------- CLONE_THREAD: create a new guest thread in the same VM ---------- */

/* Arguments passed to the worker pthread. Allocated by sys_clone_thread,
 * freed by the worker after vCPU creation and register setup. */
typedef struct {
    thread_entry_t *thread;
    guest_t        *guest;
    int             verbose;
    uint64_t        child_stack;
    uint64_t        flags;
    uint64_t        tls;
    /* Parent system regs to copy into the new vCPU */
    uint64_t        elr, spsr, vbar, ttbr0, ttbr1, sctlr, tcr, mair, cpacr;
    uint64_t        tpidr;
    uint64_t        gprs[31];
    uint64_t        sp_el1;
} thread_create_args_t;

/* Forward declaration — worker entry runs after sys_clone_thread */
static void *thread_create_and_run(void *arg);

static int64_t sys_clone_thread(hv_vcpu_t parent_vcpu, guest_t *g,
                                uint64_t flags, uint64_t child_stack,
                                uint64_t ptid_gva, uint64_t tls,
                                uint64_t ctid_gva, int verbose) {
    /* Allocate guest TID */
    int64_t child_tid = proc_alloc_pid();

    /* Allocate thread table slot */
    thread_entry_t *t = thread_alloc(child_tid);
    if (!t) {
        fprintf(stderr, "hl: clone_thread: thread table full\n");
        return -LINUX_EAGAIN;
    }

    /* Inherit parent's signal mask (POSIX: clone inherits blocked mask) */
    if (current_thread)
        t->blocked = current_thread->blocked;

    /* Allocate per-thread EL1 stack */
    uint64_t child_sp_el1 = thread_alloc_sp_el1();
    if (child_sp_el1 == 0) {
        thread_deactivate(t);
        return -LINUX_ENOMEM;
    }
    t->sp_el1 = child_sp_el1;

    /* Capture parent register state before spawning worker.
     * HVF binds vCPU to the creating thread, so the worker must call
     * hv_vcpu_create itself. We pass all parent state via the args. */
    uint64_t parent_elr, parent_spsr, parent_vbar, parent_ttbr0, parent_ttbr1;
    uint64_t parent_sctlr, parent_tcr, parent_mair, parent_cpacr;
    uint64_t parent_tpidr;
    parent_elr    = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_ELR_EL1);
    parent_spsr   = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_SPSR_EL1);
    parent_vbar   = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_VBAR_EL1);
    parent_ttbr0  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_TTBR0_EL1);
    parent_ttbr1  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_TTBR1_EL1);
    parent_sctlr  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_SCTLR_EL1);
    parent_tcr    = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_TCR_EL1);
    parent_mair   = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_MAIR_EL1);
    parent_cpacr  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_CPACR_EL1);
    parent_tpidr  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_TPIDR_EL0);

    uint64_t parent_gprs[31];
    for (int i = 0; i < 31; i++)
        parent_gprs[i] = vcpu_get_gpr(parent_vcpu, (unsigned)i);

    thread_create_args_t *tca = calloc(1, sizeof(thread_create_args_t));
    if (!tca) {
        thread_deactivate(t);
        return -LINUX_ENOMEM;
    }

    tca->thread      = t;
    tca->guest       = g;
    tca->verbose     = verbose;
    tca->child_stack = child_stack;
    tca->flags       = flags;
    tca->tls         = tls;
    tca->elr         = parent_elr;
    tca->spsr        = parent_spsr;
    tca->vbar        = parent_vbar;
    tca->ttbr0       = parent_ttbr0;
    tca->ttbr1       = parent_ttbr1;
    tca->sctlr       = parent_sctlr;
    tca->tcr         = parent_tcr;
    tca->mair        = parent_mair;
    tca->cpacr       = parent_cpacr;
    tca->tpidr       = parent_tpidr;
    memcpy(tca->gprs, parent_gprs, sizeof(parent_gprs));
    tca->sp_el1      = child_sp_el1;

    /* CLONE_PARENT_SETTID: write child TID to parent's ptid address */
    if (flags & LINUX_CLONE_PARENT_SETTID) {
        int32_t tid32 = (int32_t)child_tid;
        if (guest_write(g, ptid_gva, &tid32, sizeof(tid32)) < 0) {
            free(tca);
            thread_deactivate(t);
            return -LINUX_EFAULT;
        }
    }

    /* CLONE_CHILD_CLEARTID: store the address for cleanup on exit */
    if (flags & LINUX_CLONE_CHILD_CLEARTID) {
        t->clear_child_tid = ctid_gva;
    }

    /* CLONE_CHILD_SETTID: write child TID to the child's ctid address.
     * This writes into shared guest memory (visible to child thread). */
    if (flags & LINUX_CLONE_CHILD_SETTID) {
        int32_t tid32 = (int32_t)child_tid;
        if (guest_write(g, ctid_gva, &tid32, sizeof(tid32)) < 0) {
            free(tca);
            thread_deactivate(t);
            return -LINUX_EFAULT;
        }
    }

    /* Create the host pthread (joinable — exit_group joins all workers
     * via thread_join_workers_cb before process exit). Threads clean up
     * their TID address via CLONE_CHILD_CLEARTID + futex wake.
     *
     * macOS default secondary-thread stack is only 512KB. Several syscall
     * handlers still use large on-stack buffers (e.g. 64KB I/O), and the
     * main thread's 8MB RLIMIT_STACK does not apply here — give workers
     * 2MB so they match typical Linux pthread defaults. */
    pthread_t host_thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    (void)pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);

    int err = pthread_create(&host_thread, &attr, thread_create_and_run, tca);
    pthread_attr_destroy(&attr);

    if (err != 0) {
        fprintf(stderr, "hl: clone_thread: pthread_create failed: %s\n",
                strerror(err));
        free(tca);
        thread_deactivate(t);
        return -LINUX_EAGAIN;
    }

    t->host_thread = host_thread;

    if (verbose)
        fprintf(stderr, "hl: clone_thread: child tid=%lld created\n",
                (long long)child_tid);

    return child_tid;
}

/* Worker pthread entry: creates the HVF vCPU on this thread (required by
 * Apple HVF — vCPU is bound to the creating thread), configures all
 * registers from parent state, then enters the run loop. On exit,
 * performs CLONE_CHILD_CLEARTID cleanup (write 0 + FUTEX_WAKE). */
static void *thread_create_and_run(void *arg) {
    thread_create_args_t *tca = (thread_create_args_t *)arg;
    thread_entry_t *t = tca->thread;
    guest_t *g = tca->guest;

    /* Create vCPU on THIS thread (HVF requirement) */
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    hv_return_t r = hv_vcpu_create(&vcpu, &vexit, NULL);
    if (r != HV_SUCCESS) {
        fprintf(stderr, "hl: thread tid=%lld: hv_vcpu_create failed: %d\n",
                (long long)t->guest_tid, (int)r);
        free(tca);
        thread_deactivate(t);
        return NULL;
    }

    t->vcpu  = vcpu;
    t->vexit = vexit;

    /* Copy system registers from parent (shared page tables, same MMU config) */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, tca->vbar));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, tca->mair));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1,  tca->tcr));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, tca->ttbr0));
    if (tca->ttbr1)
        HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR1_EL1, tca->ttbr1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, tca->cpacr));

    /* MMU already on — set SCTLR with M=1 directly (page tables exist) */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, tca->sctlr));

    /* Per-thread SP_EL1 (each vCPU needs its own EL1 exception stack) */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL1, tca->sp_el1));

    /* SP_EL0 = child_stack (provided by clone caller) */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, tca->child_stack));

    /* TPIDR_EL0 = thread-local storage pointer (if CLONE_SETTLS) */
    if (tca->flags & LINUX_CLONE_SETTLS) {
        HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, tca->tls));
    } else {
        HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, tca->tpidr));
    }

    /* ELR_EL1 = clone return point (same as parent) */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, tca->elr));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, tca->spsr));

    /* Copy all 31 GPRs from parent, then set X0=0 (child clone return) */
    for (int i = 0; i < 31; i++)
        HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, tca->gprs[i]));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, 0));

    /* Start at clone return point in EL0 (not shim entry) */
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, tca->elr));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0)); /* EL0t */

    int verbose = tca->verbose;
    free(tca);

    /* Set per-thread TLS pointer and enter worker run loop */
    current_thread = t;

    if (verbose)
        fprintf(stderr, "hl: thread tid=%lld starting on vCPU\n",
                (long long)t->guest_tid);

    vcpu_run_loop(vcpu, vexit, g, verbose, 0, 0 /* worker */);

    /* CLONE_CHILD_CLEARTID: write 0 to the address and wake one waiter.
     * This is how pthread_join works in musl — the joining thread does
     * FUTEX_WAIT on this address until it becomes 0. */
    if (t->clear_child_tid != 0) {
        uint32_t zero = 0;
        if (guest_write(g, t->clear_child_tid, &zero, sizeof(zero)) == 0) {
            futex_wake_one(g, t->clear_child_tid);
        } else {
            fprintf(stderr, "hl: warning: thread tid=%lld clear_child_tid "
                    "write failed (gva=0x%llx)\n",
                    (long long)t->guest_tid,
                    (unsigned long long)t->clear_child_tid);
        }
    }

    if (verbose)
        fprintf(stderr, "hl: thread tid=%lld exiting\n",
                (long long)t->guest_tid);

    hv_vcpu_destroy(vcpu);
    thread_deactivate(t);

    /* When all CLONE_THREAD workers have exited and only the main
     * thread remains, interrupt its futex_wait. Under rosetta, the
     * main thread (JIT tracer) may hang forever in an internal
     * futex_wait. In real Linux, child exit delivers SIGCHLD which
     * interrupts futex_wait with -EINTR. We simulate this via the
     * futex_interrupt_requested flag — it interrupts futex_wait
     * without triggering a full exit_group, allowing the main thread
     * to continue processing and call exit_group with the correct
     * exit code naturally. */
    if (thread_active_count() == 1) {
        if (verbose)
            fprintf(stderr, "hl: last worker exited, interrupting "
                    "main thread futex_wait/poll\n");
        futex_interrupt_broadcast();
        wakeup_pipe_signal();
        thread_interrupt_all();
    }

    return NULL;
}

/* ---------- CLONE_VM: create a new thread sharing guest memory, waitable via wait4 ---------- */

/* Worker entry for vm-clone child threads. Nearly identical to
 * thread_create_and_run but sets vm-clone exit semantics. */
static void *vm_clone_thread_run(void *arg);

static int64_t sys_clone_vm(hv_vcpu_t parent_vcpu, guest_t *g,
                             uint64_t flags, uint64_t child_stack,
                             uint64_t ptid_gva, uint64_t tls,
                             uint64_t ctid_gva, int verbose) {
    /* Allocate guest TID */
    int64_t child_tid = proc_alloc_pid();

    /* Allocate thread table slot */
    thread_entry_t *t = thread_alloc(child_tid);
    if (!t) {
        fprintf(stderr, "hl: clone_vm: thread table full\n");
        return -LINUX_EAGAIN;
    }

    /* Mark as VM-clone child (waitable via wait4, not CLONE_THREAD) */
    t->is_vm_clone = 1;
    t->parent_tid  = current_thread ? current_thread->guest_tid : 0;
    t->exit_signal = (int)(flags & 0xFF);  /* Low byte = exit signal */
    if (t->exit_signal == 0)
        t->exit_signal = LINUX_SIGCHLD;

    /* Inherit parent's signal mask */
    if (current_thread)
        t->blocked = current_thread->blocked;

    /* Allocate per-thread EL1 stack */
    uint64_t child_sp_el1 = thread_alloc_sp_el1();
    if (child_sp_el1 == 0) {
        thread_deactivate(t);
        return -LINUX_ENOMEM;
    }
    t->sp_el1 = child_sp_el1;

    /* Capture parent register state */
    uint64_t parent_elr    = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_ELR_EL1);
    uint64_t parent_spsr   = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_SPSR_EL1);
    uint64_t parent_vbar   = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_VBAR_EL1);
    uint64_t parent_ttbr0  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_TTBR0_EL1);
    uint64_t parent_ttbr1  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_TTBR1_EL1);
    uint64_t parent_sctlr  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_SCTLR_EL1);
    uint64_t parent_tcr    = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_TCR_EL1);
    uint64_t parent_mair   = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_MAIR_EL1);
    uint64_t parent_cpacr  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_CPACR_EL1);
    uint64_t parent_tpidr  = vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_TPIDR_EL0);

    uint64_t parent_gprs[31];
    for (int i = 0; i < 31; i++)
        parent_gprs[i] = vcpu_get_gpr(parent_vcpu, (unsigned)i);

    thread_create_args_t *tca = calloc(1, sizeof(thread_create_args_t));
    if (!tca) {
        thread_deactivate(t);
        return -LINUX_ENOMEM;
    }

    tca->thread      = t;
    tca->guest       = g;
    tca->verbose     = verbose;
    tca->child_stack = child_stack ? child_stack :
                       vcpu_get_sysreg(parent_vcpu, HV_SYS_REG_SP_EL0);
    tca->flags       = flags;
    tca->tls         = tls;
    tca->elr         = parent_elr;
    tca->spsr        = parent_spsr;
    tca->vbar        = parent_vbar;
    tca->ttbr0       = parent_ttbr0;
    tca->ttbr1       = parent_ttbr1;
    tca->sctlr       = parent_sctlr;
    tca->tcr         = parent_tcr;
    tca->mair        = parent_mair;
    tca->cpacr       = parent_cpacr;
    tca->tpidr       = parent_tpidr;
    memcpy(tca->gprs, parent_gprs, sizeof(parent_gprs));
    tca->sp_el1      = child_sp_el1;

    /* CLONE_PARENT_SETTID: write child TID to parent's ptid address */
    if (flags & LINUX_CLONE_PARENT_SETTID) {
        int32_t tid32 = (int32_t)child_tid;
        if (guest_write(g, ptid_gva, &tid32, sizeof(tid32)) < 0) {
            free(tca);
            thread_deactivate(t);
            return -LINUX_EFAULT;
        }
    }

    /* CLONE_CHILD_CLEARTID: store the address for cleanup on exit */
    if (flags & LINUX_CLONE_CHILD_CLEARTID)
        t->clear_child_tid = ctid_gva;

    /* CLONE_CHILD_SETTID: write child TID to child's ctid address */
    if (flags & LINUX_CLONE_CHILD_SETTID) {
        int32_t tid32 = (int32_t)child_tid;
        if (guest_write(g, ctid_gva, &tid32, sizeof(tid32)) < 0) {
            free(tca);
            thread_deactivate(t);
            return -LINUX_EFAULT;
        }
    }

    /* Create the host pthread (2MB stack — see sys_clone_thread comment) */
    pthread_t host_thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    (void)pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);

    int err = pthread_create(&host_thread, &attr, vm_clone_thread_run, tca);
    pthread_attr_destroy(&attr);

    if (err != 0) {
        fprintf(stderr, "hl: clone_vm: pthread_create failed: %s\n",
                strerror(err));
        free(tca);
        thread_deactivate(t);
        return -LINUX_EAGAIN;
    }

    t->host_thread = host_thread;

    if (verbose)
        fprintf(stderr, "hl: clone_vm: child tid=%lld created "
                "(parent=%lld, flags=0x%llx)\n",
                (long long)child_tid, (long long)t->parent_tid,
                (unsigned long long)flags);

    return child_tid;
}

/* Worker entry for vm-clone children. Sets up vCPU, runs guest code,
 * then marks exit status for parent's wait4 to collect. */
static void *vm_clone_thread_run(void *arg) {
    thread_create_args_t *tca = (thread_create_args_t *)arg;
    thread_entry_t *t = tca->thread;
    guest_t *g = tca->guest;

    /* Create vCPU on THIS thread (HVF requirement) */
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    hv_return_t r = hv_vcpu_create(&vcpu, &vexit, NULL);
    if (r != HV_SUCCESS) {
        fprintf(stderr, "hl: vm_clone tid=%lld: hv_vcpu_create failed: %d\n",
                (long long)t->guest_tid, (int)r);
        free(tca);
        thread_deactivate(t);
        return NULL;
    }

    t->vcpu  = vcpu;
    t->vexit = vexit;

    /* Copy system registers from parent */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, tca->vbar));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, tca->mair));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1,  tca->tcr));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, tca->ttbr0));
    if (tca->ttbr1)
        HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR1_EL1, tca->ttbr1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, tca->cpacr));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, tca->sctlr));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL1, tca->sp_el1));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, tca->child_stack));

    /* TLS pointer */
    if (tca->flags & LINUX_CLONE_SETTLS) {
        HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, tca->tls));
    } else {
        HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, tca->tpidr));
    }

    /* ELR_EL1 = clone return point */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, tca->elr));
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, tca->spsr));

    /* Copy all 31 GPRs from parent, set X0=0 (child clone return) */
    for (int i = 0; i < 31; i++)
        HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, tca->gprs[i]));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, 0));

    /* Start at clone return point in EL0 */
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, tca->elr));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0)); /* EL0t */

    int verbose = tca->verbose;
    free(tca);

    /* Set per-thread TLS pointer and enter worker run loop */
    current_thread = t;

    if (verbose)
        fprintf(stderr, "hl: vm_clone tid=%lld starting on vCPU\n",
                (long long)t->guest_tid);

    int exit_code = vcpu_run_loop(vcpu, vexit, g, verbose, 0, 0 /* worker */);

    /* CLONE_CHILD_CLEARTID cleanup */
    if (t->clear_child_tid != 0) {
        uint32_t zero = 0;
        if (guest_write(g, t->clear_child_tid, &zero, sizeof(zero)) == 0)
            futex_wake_one(g, t->clear_child_tid);
    }

    /* Mark exit status for parent's wait4 to collect.
     * vm_exit_status uses wait-format: (exit_code << 8) for normal exit. */
    pthread_mutex_t *lock = thread_get_lock();
    pthread_mutex_lock(lock);
    t->vm_exited = 1;
    t->vm_exit_status = (exit_code & 0xFF) << 8;
    pthread_cond_broadcast(&t->ptrace_cond);
    pthread_mutex_unlock(lock);

    if (verbose)
        fprintf(stderr, "hl: vm_clone tid=%lld exiting (code=%d)\n",
                (long long)t->guest_tid, exit_code);

    /* Check if this was the last VM-clone child BEFORE destroying the
     * vCPU — thread_interrupt_all needs valid vCPU handles. In real
     * Linux, child exit delivers exit_signal (SIGCHLD) which interrupts
     * the parent's futex_wait with -EINTR. We simulate this by
     * requesting exit_group and interrupting all vCPUs. */
    int last_clone = (thread_count_active_vm_clones() == 0);

    if (last_clone) {
        if (verbose)
            fprintf(stderr, "hl: last vm_clone exited, triggering exit_group\n");
        atomic_store(&exit_group_requested, 1);
        atomic_store(&exit_group_code, exit_code);
        /* Interrupt all vCPUs while ours is still valid. The main
         * thread's vCPU may be blocked in hv_vcpu_run — this forces
         * it out so it can check exit_group_requested. Our own vCPU
         * is not in hv_vcpu_run (loop already exited) so the exit
         * call on it is a harmless no-op. */
        thread_interrupt_all();
        /* hv_vcpus_exit() only breaks a thread out of hv_vcpu_run(); it does
         * nothing for one parked in host poll()/pselect(). Infinite
         * ppoll/pselect6 wait with no timeout slice when the wake pipe is
         * present, so without this nudge exit_group_requested is never
         * observed and the process hangs forever. sys_exit_group pairs
         * these two the same way. */
        wakeup_pipe_signal();
    }

    hv_vcpu_destroy(vcpu);
    /* Don't deactivate yet — parent needs to wait4 to collect status.
     * The slot is freed when thread_ptrace_wait reads vm_exited. */

    return NULL;
}

/* posix_spawn + IPC path for true process clone (not CLONE_THREAD/CLONE_VM).
 *
 * Kept as a separate noinline function so its locals (path buffers, register
 * snapshots) are NOT allocated on every pthread_create-backed CLONE_THREAD
 * call. Worker vCPU threads only get the macOS default ~512KB stack; the
 * pre-fix combined frame for this path was ~900KB (FD table + VFS mount
 * arrays on stack) and host-SIGBUS'd (___chkstk_darwin) when mpg123 cloned
 * a decode thread from a worker. Large arrays are heap-allocated below. */
__attribute__((noinline))
static int64_t sys_clone_fork(hv_vcpu_t vcpu, guest_t *g, uint64_t flags,
                              int verbose) {
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
    char self_path[LINUX_PATH_MAX];
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
    char *child_argv[12];
    int ci = 0;
    child_argv[ci++] = self_path;
    if (verbose) child_argv[ci++] = "--verbose";
    /* The backend is process-local state, not part of the IPC header, and
     * fork_child_main() returns before hl.c applies profile defaults — so a
     * child of `hl --audio-backend null` used to fall back to the compiled
     * default and open a real Core Audio queue. Pass it through. */
    child_argv[ci++] = "--audio-backend";
    child_argv[ci++] = (char *)hl_audio_backend_name();
    /* Inherit the parent's opt-in watchdog. Absent → child defaults to off. */
    char timeout_str[16];
    if (hl_watchdog_timeout_sec > 0) {
        snprintf(timeout_str, sizeof(timeout_str), "%d", hl_watchdog_timeout_sec);
        child_argv[ci++] = "--timeout";
        child_argv[ci++] = timeout_str;
    }
    child_argv[ci++] = "--fork-child";
    child_argv[ci++] = fd_str;
    child_argv[ci] = NULL;

    /* Set up spawn attributes — close all inherited FDs by default.
     * POSIX_SPAWN_CLOEXEC_DEFAULT (macOS extension) marks all FDs as
     * close-on-exec in the child. Without this, ALL parent host FDs
     * (pipes, sockets, etc.) leak into the child hl process, wasting
     * file descriptors and potentially preventing pipe EOF detection. */
    posix_spawnattr_t spawn_attr;
    posix_spawnattr_init(&spawn_attr);
    posix_spawnattr_setflags(&spawn_attr, POSIX_SPAWN_CLOEXEC_DEFAULT);

    /* Set up file actions — explicitly inherit only needed FDs.
     * With CLOEXEC_DEFAULT, everything is closed unless we opt in. */
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_addinherit_np(&file_actions, STDIN_FILENO);
    posix_spawn_file_actions_addinherit_np(&file_actions, STDOUT_FILENO);
    posix_spawn_file_actions_addinherit_np(&file_actions, STDERR_FILENO);
    posix_spawn_file_actions_addinherit_np(&file_actions, sock_fds[1]);

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
    int ipc_sock = sock_fds[0];

    /* Step 4: Assign guest PID to child */
    int64_t child_guest_pid = proc_alloc_pid();

    /* Step 5: Serialize state to child */

    /* Determine if we can use the COW (shm) fast path.
     * If shm_fd >= 0, we freeze a snapshot via MAP_PRIVATE and send the
     * shm fd to the child. Otherwise fall back to region-by-region copy. */
    /* Use COW fork when the guest has file-backed shared memory.
     * Disabled for rosetta (x86_64) mode: rosetta's JIT runtime maintains
     * process-local state (TLS, code caches, slab allocators) that can't
     * survive a snapshot — the child's rosetta would resume with the parent's
     * stale JIT state, causing corrupted translations. The legacy IPC path
     * copies only semantic memory regions, allowing rosetta to reinitialize
     * its JIT cleanly in the child. */
    int use_shm = (g->shm_fd >= 0) && !g->is_rosetta;

    /* Note: we do NOT remap the parent to MAP_PRIVATE here. The parent
     * stays on MAP_SHARED — its vCPU continues writing to the shared file.
     * The child maps MAP_PRIVATE, getting a COW snapshot.
     *
     * This is safe because: the IPC is synchronous — the child maps
     * MAP_PRIVATE before the parent's vCPU resumes. After that, the
     * child's COW pages are frozen (child writes are private, parent
     * writes to MAP_SHARED don't affect COW'd child pages).
     *
     * We previously tried remapping the parent to MAP_PRIVATE here, but
     * that breaks HVF: hv_vm_map caches the host VA→PA mapping, and
     * MAP_FIXED remap invalidates it. The parent's vCPU then reads stale
     * memory, causing corrupted syscall data (EFAULT on writev). */

    /* Header */
    ipc_header_t hdr = {
        .magic = IPC_MAGIC_HEADER,
        .version = IPC_VERSION,
        .ipa_bits = g->ipa_bits,
        .has_shm = (uint32_t)use_shm,
        .child_pid = child_guest_pid,
        .parent_pid = proc_get_pid(),
        .guest_size = g->guest_size,
        .brk_base = g->brk_base,
        .brk_current = g->brk_current,
        .stack_base = g->stack_base,
        .stack_top = g->stack_top,
        .mmap_next = g->mmap_next,
        .mmap_end = g->mmap_end,
        .pt_pool_next = g->pt_pool_next,
        .ttbr0 = g->ttbr0,
        /* Rosetta state */
        .is_rosetta = (uint32_t)g->is_rosetta,
        .ttbr1 = g->ttbr1,
        .kbuf_gpa = g->kbuf_gpa,
        .mmap_rx_next = g->mmap_rx_next,
        .mmap_rx_end = g->mmap_rx_end,
        .rosetta_guest_base = g->rosetta_guest_base,
        .rosetta_va_base    = g->rosetta_va_base,
        .rosetta_size       = g->rosetta_size,
    };
    if (ipc_write_all(ipc_sock, &hdr, sizeof(hdr)) < 0) {
        fprintf(stderr, "hl: clone: failed to send header\n");
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }

    /* COW path: send shm fd to child via SCM_RIGHTS */
    if (use_shm) {
        if (send_fds(ipc_sock, &g->shm_fd, 1) < 0) {
            fprintf(stderr, "hl: clone: failed to send shm fd\n");
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
    }

    /* Registers -- capture current vCPU state */
    ipc_registers_t regs = {0};
    regs.elr_el1   = vcpu_get_sysreg(vcpu, HV_SYS_REG_ELR_EL1);
    regs.sp_el0    = vcpu_get_sysreg(vcpu, HV_SYS_REG_SP_EL0);
    regs.spsr_el1  = vcpu_get_sysreg(vcpu, HV_SYS_REG_SPSR_EL1);
    regs.vbar_el1  = vcpu_get_sysreg(vcpu, HV_SYS_REG_VBAR_EL1);
    regs.ttbr0_el1 = vcpu_get_sysreg(vcpu, HV_SYS_REG_TTBR0_EL1);
    regs.sctlr_el1 = vcpu_get_sysreg(vcpu, HV_SYS_REG_SCTLR_EL1);
    regs.tcr_el1   = vcpu_get_sysreg(vcpu, HV_SYS_REG_TCR_EL1);
    regs.mair_el1  = vcpu_get_sysreg(vcpu, HV_SYS_REG_MAIR_EL1);
    regs.cpacr_el1 = vcpu_get_sysreg(vcpu, HV_SYS_REG_CPACR_EL1);
    regs.tpidr_el0 = vcpu_get_sysreg(vcpu, HV_SYS_REG_TPIDR_EL0);
    regs.sp_el1    = vcpu_get_sysreg(vcpu, HV_SYS_REG_SP_EL1);
    regs.ttbr1_el1 = vcpu_get_sysreg(vcpu, HV_SYS_REG_TTBR1_EL1);
    for (int i = 0; i < 31; i++)
        regs.x[i] = vcpu_get_gpr(vcpu, (unsigned)i);

    if (ipc_write_all(ipc_sock, &regs, sizeof(regs)) < 0) {
        fprintf(stderr, "hl: clone: failed to send registers\n");
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }

    /* Memory regions — skipped entirely in COW path (child has full snapshot) */
    if (use_shm) {
        /* Send 0 regions — child already has all memory via COW */
        uint32_t zero_regions = 0;
        if (ipc_write_all(ipc_sock, &zero_regions, sizeof(zero_regions)) < 0) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
    } else {
        /* Legacy path: enumerate and send used memory regions.
         * Hold mmap_lock during enumeration to get a consistent snapshot
         * of the allocator state (brk_current, mmap_next, etc.). Release
         * before data send to avoid blocking other threads for too long.
         * Lock order: mmap_lock(1) — acquired before fd_lock(3) below. */
        #define MAX_USED_REGIONS 16
        used_region_t used[MAX_USED_REGIONS];
        unsigned int shim_sz = proc_get_shim_size();
        pthread_mutex_lock(&mmap_lock);
        int nregions = guest_get_used_regions(g, shim_sz, used, MAX_USED_REGIONS);
        pthread_mutex_unlock(&mmap_lock);
        uint32_t num_regions = (uint32_t)nregions;
        if (ipc_write_all(ipc_sock, &num_regions, sizeof(num_regions)) < 0) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }

        for (int i = 0; i < nregions; i++) {
            ipc_region_header_t rhdr = {
                .offset = used[i].offset,
                .size = used[i].size,
            };
            if (ipc_write_all(ipc_sock, &rhdr, sizeof(rhdr)) < 0) {
                close(ipc_sock);
                return -LINUX_ENOMEM;
            }

            /* Send region data in 1MB chunks */
            uint8_t *src = (uint8_t *)g->host_base + used[i].offset;
            size_t remaining = used[i].size;
            while (remaining > 0) {
                size_t chunk = remaining > ((size_t)1024 * 1024) ? ((size_t)1024 * 1024) : remaining;
                if (ipc_write_all(ipc_sock, src, chunk) < 0) {
                    close(ipc_sock);
                    return -LINUX_ENOMEM;
                }
                src += chunk;
                remaining -= chunk;
            }
        }
    }

    /* FD table -- count open fds and send entries + host fds via SCM_RIGHTS.
     * Note: CLOEXEC FDs are inherited across fork (POSIX semantics).
     * CLOEXEC only takes effect at exec (handled in syscall_exec.c).
     *
     * Heap-allocate the FD_TABLE_SIZE snapshots: on-stack they alone are
     * hundreds of KB and would blow the 512KB default pthread stack if this
     * path were ever reached from a worker vCPU thread. */
    ipc_fd_entry_t *fd_entries = calloc(FD_TABLE_SIZE, sizeof(*fd_entries));
    int *host_fds_to_send = calloc(FD_TABLE_SIZE, sizeof(*host_fds_to_send));
    /* Track which host_fds were duped (need close) vs passed as-is (STDIO) */
    int *host_fds_duped = calloc(FD_TABLE_SIZE, sizeof(*host_fds_duped));
    hl_fork_object_record_t *obj_records =
        calloc(FD_TABLE_SIZE, sizeof(*obj_records));
    int32_t *obj_gfds = calloc(FD_TABLE_SIZE, sizeof(*obj_gfds));
    /* Snapshot of the table taken under fd_lock. Everything that can block
     * (the OSS export, which takes the audio stream mutex, and open()/dup())
     * runs afterwards with the lock released. */
    struct fd_scan {
        int gfd;
        int type;
        int linux_flags;
        int host_fd;
        int dev_index;        /* FD_DEVICE registry index, else -1 */
        hl_open_file_t *of;   /* retained when set; released after use */
    } *scan = calloc(FD_TABLE_SIZE, sizeof(*scan));
    if (!fd_entries || !host_fds_to_send || !host_fds_duped ||
        !obj_records || !obj_gfds || !scan) {
        free(fd_entries);
        free(host_fds_to_send);
        free(host_fds_duped);
        free(obj_records);
        free(obj_gfds);
        free(scan);
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }
    uint32_t num_fds = 0;
    uint32_t num_objects = 0;

    /* Pass 1: snapshot the FD table under fd_lock. Nothing here blocks.
     *
     * The OSS export must NOT run here: it takes the audio stream mutex,
     * which CLAUDE.md documents as a leaf lock to be taken only after
     * releasing fd_lock. A writer parked in hl_audio_stream_write() holds
     * that mutex across a wait that only the Core Audio callback ends, so
     * exporting under fd_lock could block every FD syscall on every vCPU
     * thread indefinitely. Retain the open-file so it stays alive once the
     * lock is dropped. */
    int nscan = 0;
    pthread_mutex_lock(&fd_lock);
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fd_table[i].type == FD_CLOSED) continue;
        scan[nscan].gfd         = i;
        scan[nscan].type        = fd_table[i].type;
        scan[nscan].linux_flags = fd_table[i].linux_flags;
        scan[nscan].host_fd     = fd_table[i].host_fd;
        scan[nscan].dev_index   = (fd_table[i].type == FD_DEVICE)
                                ? hl_device_node_index(fd_table[i].dir) : -1;
        scan[nscan].of          = NULL;
        if (fd_table[i].of && hl_oss_fd_needs_recreate(fd_table[i].type)) {
            scan[nscan].of = fd_table[i].of;
            hl_open_file_retain(scan[nscan].of);
        }
        nscan++;
    }
    pthread_mutex_unlock(&fd_lock);

    /* Pass 2: blocking work with fd_lock released. */
    for (int s = 0; s < nscan; s++) {
        int i = scan[s].gfd;
        int host_fd;
        int was_duped = 0;
        int has_object = 0;

        if (scan[s].of) {
            /* OSS / synthetic: export open-file config; do NOT share stream FDs.
             * Take the placeholder fd first — committing num_objects before it
             * could leave an object record with no matching FD entry. */
            hl_fork_object_record_t rec;
            if (hl_oss_fork_export(scan[s].of, &rec) != 0) {
                hl_open_file_release(scan[s].of);
                continue;
            }
            host_fd = open("/dev/null", O_RDWR);
            if (host_fd < 0) {
                hl_open_file_release(scan[s].of);
                continue;
            }
            has_object = 1;
            obj_gfds[num_objects] = i;
            obj_records[num_objects] = rec;
            num_objects++;
            was_duped = 1;
            hl_open_file_release(scan[s].of);
        } else if (scan[s].type != FD_STDIO) {
            /* Dup the fd so child gets its own copy */
            if (scan[s].host_fd < 0) continue;
            int duped = dup(scan[s].host_fd);
            if (duped < 0) continue; /* Skip entry if dup fails */
            host_fd = duped;
            was_duped = 1;
        } else {
            /* For stdio, send the actual fd (0, 1, 2) */
            host_fd = scan[s].host_fd;
        }

        fd_entries[num_fds].guest_fd = i;
        fd_entries[num_fds].type = scan[s].type;
        fd_entries[num_fds].linux_flags = scan[s].linux_flags;
        /* FD_DEVICE never has an open-file object, so `pad` is free to carry
         * the registry-node index — the child re-derives its .dir from it so
         * fstat on an inherited /dev fd keeps the Linux device numbers (V6).
         * -1 stays -1 (no node). */
        if (scan[s].type == FD_DEVICE)
            fd_entries[num_fds].pad = scan[s].dev_index;
        else
            fd_entries[num_fds].pad = has_object ? IPC_FD_HAS_OBJECT : 0;
        host_fds_to_send[num_fds] = host_fd;
        host_fds_duped[num_fds] = was_duped;
        num_fds++;
    }
    free(scan);
    scan = NULL;
    int num_host_fds = (int)num_fds; /* 1:1 with fd_entries */

    if (ipc_write_all(ipc_sock, &num_fds, sizeof(num_fds)) < 0) {
        for (uint32_t fi = 0; fi < num_fds; fi++)
            if (host_fds_duped[fi]) close(host_fds_to_send[fi]);
        free(fd_entries);
        free(host_fds_to_send);
        free(host_fds_duped);
        free(obj_records);
        free(obj_gfds);
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }

    if (num_fds > 0) {
        if (ipc_write_all(ipc_sock, fd_entries, num_fds * sizeof(ipc_fd_entry_t)) < 0) {
            for (uint32_t fi = 0; fi < num_fds; fi++)
                if (host_fds_duped[fi]) close(host_fds_to_send[fi]);
            free(fd_entries);
            free(host_fds_to_send);
            free(host_fds_duped);
            free(obj_records);
            free(obj_gfds);
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }

        /* Send host FDs via SCM_RIGHTS */
        if (send_fds(ipc_sock, host_fds_to_send, num_host_fds) < 0) {
            fprintf(stderr, "hl: clone: failed to send fds via SCM_RIGHTS\n");
            for (uint32_t fi = 0; fi < num_fds; fi++) {
                if (host_fds_duped[fi])
                    close(host_fds_to_send[fi]);
            }
            free(fd_entries);
            free(host_fds_to_send);
            free(host_fds_duped);
            free(obj_records);
            free(obj_gfds);
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }

        /* Close duped fds in parent.  The host_fds_duped[] array
         * tracks which entries were dup()'d (1:1 with fd_entries). */
        for (uint32_t fi = 0; fi < num_fds; fi++) {
            if (host_fds_duped[fi])
                close(host_fds_to_send[fi]);
        }
    }

    /* IPC v5 object graph after SCM_RIGHTS (even if num_fds==0 send count). */
    if (ipc_write_all(ipc_sock, &num_objects, sizeof(num_objects)) < 0) {
        free(fd_entries);
        free(host_fds_to_send);
        free(host_fds_duped);
        free(obj_records);
        free(obj_gfds);
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }
    for (uint32_t oi = 0; oi < num_objects; oi++) {
        if (ipc_write_all(ipc_sock, &obj_gfds[oi], sizeof(obj_gfds[oi])) < 0 ||
            ipc_write_all(ipc_sock, &obj_records[oi], sizeof(obj_records[oi])) < 0) {
            free(fd_entries);
            free(host_fds_to_send);
            free(host_fds_duped);
            free(obj_records);
            free(obj_gfds);
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
        if (hl_trace_on(HL_TRACE_FORK))
            hl_trace(HL_TRACE_FORK, "export gfd=%d kind=%u policy=recreate-empty",
                     (int)obj_gfds[oi], obj_records[oi].kind);
    }
    free(fd_entries);
    free(host_fds_to_send);
    free(host_fds_duped);
    free(obj_records);
    free(obj_gfds);
    fd_entries = NULL;
    host_fds_to_send = NULL;
    host_fds_duped = NULL;
    obj_records = NULL;
    obj_gfds = NULL;

    /* Process info: cwd + umask (+ VFS config for rooted mode) */
    char cwd[LINUX_PATH_MAX] = {0};
    if (hl_vfs_mode() == HL_FS_ROOTED) {
        const char *gcwd = hl_vfs_cwd();
        if (gcwd) snprintf(cwd, sizeof(cwd), "%s", gcwd);
    } else {
        getcwd(cwd, sizeof(cwd));
    }
    mode_t cur_umask = umask(0);
    umask(cur_umask);
    uint32_t umask_val = (uint32_t)cur_umask;

    if (ipc_write_all(ipc_sock, cwd, sizeof(cwd)) < 0 ||
        ipc_write_all(ipc_sock, &umask_val, sizeof(umask_val)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }

    /* VFS config reconstruction (mounts + mode + cwd). Heap-allocate:
     * hl_vfs_t is ~0.5MB (64 mounts × path pairs) — too big for worker stacks. */
    {
        hl_vfs_t *vfs_snap = calloc(1, sizeof(*vfs_snap));
        if (!vfs_snap) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
        hl_vfs_fork_export(vfs_snap);
        int werr = ipc_write_all(ipc_sock, vfs_snap, sizeof(*vfs_snap));
        free(vfs_snap);
        if (werr < 0) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
    }

    /* Attached SysV SHM segments. They must stay SHARED across fork, but the
     * Stage-2 override lives in the parent's VM only and the segment table is
     * process-local, so without this the child silently resolved SHM guest
     * VAs to its own COW copy of primary RAM. */
    {
        int shm_max = hl_shm_fork_max();
        hl_shm_fork_rec_t *shm_recs =
            calloc((size_t)shm_max, sizeof(*shm_recs));
        if (!shm_recs) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
        int32_t nshm = (int32_t)hl_shm_fork_export(shm_recs, shm_max);
        if (ipc_write_all(ipc_sock, &nshm, sizeof(nshm)) < 0 ||
            (nshm > 0 && ipc_write_all(ipc_sock, shm_recs,
                                       (size_t)nshm * sizeof(*shm_recs)) < 0)) {
            free(shm_recs);
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
        free(shm_recs);
    }

    /* Sysroot path (for dynamic linker library resolution) */
    char sysroot_ipc[LINUX_PATH_MAX] = {0};
    const char *sr = proc_get_sysroot();
    if (sr) strncpy(sysroot_ipc, sr, sizeof(sysroot_ipc) - 1);
    if (ipc_write_all(ipc_sock, sysroot_ipc, sizeof(sysroot_ipc)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }

    /* ELF path (for /proc/self/exe) and hl path (for rosettad spawn) */
    char elf_path_ipc[LINUX_PATH_MAX] = {0};
    const char *ep = proc_get_elf_path();
    if (ep) strncpy(elf_path_ipc, ep, sizeof(elf_path_ipc) - 1);
    char hl_path_ipc[LINUX_PATH_MAX] = {0};
    const char *hp = proc_get_hl_path();
    if (hp) strncpy(hl_path_ipc, hp, sizeof(hl_path_ipc) - 1);
    if (ipc_write_all(ipc_sock, elf_path_ipc, sizeof(elf_path_ipc)) < 0 ||
        ipc_write_all(ipc_sock, hl_path_ipc, sizeof(hl_path_ipc)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }

    /* Cmdline (for /proc/self/cmdline) */
    size_t cmdline_len = 0;
    const char *cmdline = proc_get_cmdline(&cmdline_len);
    uint32_t cmdline_len_u32 = (uint32_t)cmdline_len;
    if (ipc_write_all(ipc_sock, &cmdline_len_u32, sizeof(cmdline_len_u32)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }
    if (cmdline_len_u32 > 0 && cmdline) {
        if (ipc_write_all(ipc_sock, cmdline, cmdline_len_u32) < 0) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
    }

    /* Semantic region tracking */
    uint32_t num_guest_regions = (uint32_t)g->nregions;
    if (ipc_write_all(ipc_sock, &num_guest_regions, sizeof(num_guest_regions)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }
    if (num_guest_regions > 0) {
        if (ipc_write_all(ipc_sock, g->regions,
                          num_guest_regions * sizeof(guest_region_t)) < 0) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
    }

    /* VA aliases (for rosetta high-VA mappings).
     * Without these, fork children in rosetta mode can't resolve
     * high virtual addresses via guest_ptr/read/write. */
    uint32_t num_aliases = (uint32_t)g->naliases;
    if (ipc_write_all(ipc_sock, &num_aliases, sizeof(num_aliases)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }
    if (num_aliases > 0) {
        if (ipc_write_all(ipc_sock, g->va_aliases,
                          num_aliases * sizeof(guest_va_alias_t)) < 0) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
    }

    /* Preannounced regions (rosetta /proc/self/maps entries) */
    uint32_t num_preannounced = (uint32_t)g->npreannounced;
    if (ipc_write_all(ipc_sock, &num_preannounced, sizeof(num_preannounced)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }
    if (num_preannounced > 0) {
        if (ipc_write_all(ipc_sock, g->preannounced,
                          num_preannounced * sizeof(guest_region_t)) < 0) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
    }

    /* Signal state */
    const signal_state_t *sig = signal_get_state();
    if (ipc_write_all(ipc_sock, sig, sizeof(signal_state_t)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }

    /* Shim blob (needed for child exec) */
    const unsigned char *shim_ptr = proc_get_shim_blob();
    uint32_t shim_size_u32 = proc_get_shim_size();
    if (ipc_write_all(ipc_sock, &shim_size_u32, sizeof(shim_size_u32)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }
    if (shim_size_u32 > 0 && shim_ptr) {
        if (ipc_write_all(ipc_sock, shim_ptr, shim_size_u32) < 0) {
            close(ipc_sock);
            return -LINUX_ENOMEM;
        }
    }

    /* Sentinel */
    uint32_t sentinel = IPC_MAGIC_SENTINEL;
    if (ipc_write_all(ipc_sock, &sentinel, sizeof(sentinel)) < 0) {
        close(ipc_sock);
        return -LINUX_ENOMEM;
    }

    close(ipc_sock);

    /* After COW fork: parent stays on MAP_SHARED (no remap was done).
     * The shm fd is kept open so subsequent forks can also use COW.
     * The child has its own MAP_PRIVATE view of the same file. */

    /* Step 6: Record child in process table */
    proc_register_child(child_host_pid, child_guest_pid);

    /* Step 7: For CLONE_VFORK, wait until child exits or execs.
     * Since we can't detect exec, just wait for child to exit for now.
     * This is correct for posix_spawn: child execs immediately. */
    if (is_vfork) {
        int status;
        waitpid(child_host_pid, &status, 0);

        /* Mark as exited in process table */
        proc_mark_child_exited(child_host_pid, status);
    }

    if (verbose)
        fprintf(stderr, "hl: clone: child pid=%lld (host=%d)\n",
                (long long)child_guest_pid, child_host_pid);

    return child_guest_pid;
}

int64_t sys_clone(hv_vcpu_t vcpu, guest_t *g, uint64_t flags,
                  uint64_t child_stack, uint64_t ptid_gva,
                  uint64_t tls, uint64_t ctid_gva, int verbose) {
    /* CLONE_THREAD: create a new thread in the same VM (not a new process).
     * Must stay a thin dispatcher — see sys_clone_fork comment about stack. */
    if (flags & LINUX_CLONE_THREAD) {
        return sys_clone_thread(vcpu, g, flags, child_stack,
                                ptid_gva, tls, ctid_gva, verbose);
    }

    /* CLONE_VM without CLONE_THREAD: create an in-process VM-clone child.
     * Used by Rosetta's two-process JIT: the inferior shares guest memory
     * but has a separate TID and is waitable via wait4/ptrace. */
    if ((flags & LINUX_CLONE_VM) && !(flags & LINUX_CLONE_THREAD)) {
        return sys_clone_vm(vcpu, g, flags, child_stack,
                            ptid_gva, tls, ctid_gva, verbose);
    }

    return sys_clone_fork(vcpu, g, flags, verbose);
}
