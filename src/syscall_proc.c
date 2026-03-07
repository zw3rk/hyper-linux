/* syscall_proc.c — Process state and management for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Owns all static process state: guest PID/PPID, shim blob reference,
 * ELF path, command line, and the process table for tracking fork
 * children. Provides accessor functions for modules that need this
 * state (fork_ipc.c, syscall_exec.c, proc_emulation.c).
 *
 * Also contains wait4, waitid, and the vCPU run loop.
 */
#include "syscall_proc.h"
#include "syscall_internal.h"
#include "syscall_signal.h"
#include "hv_util.h"
#include "crash_report.h"
#include "thread.h"
#include "futex.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/resource.h>       /* struct rusage, for wait4 rusage population */
#include <sys/sysctl.h>

/* ---------- Process state ---------- */

/* Current guest PID and PPID. Initial process is PID=1, PPID=0. */
static int64_t guest_pid = 1;
static int64_t parent_pid = 0;

/* Shim blob reference (set by proc_set_shim from hl.c) */
static const unsigned char *shim_blob_ptr = NULL;
static unsigned int shim_blob_size = 0;

/* Current ELF binary path for /proc/self/exe emulation */
static char elf_path[LINUX_PATH_MAX] = {0};

/* Guest command line for /proc/self/cmdline (NUL-separated argv) */
static char cmdline_buf[8192] = {0};
static size_t cmdline_len = 0;

/* Absolute path of the hl binary itself (for spawning sub-processes like
 * rosettad).  Set once at startup via _NSGetExecutablePath(). */
static char hl_path[LINUX_PATH_MAX] = {0};

/* Sysroot path for dynamic linker library resolution */
static char sysroot_path[LINUX_PATH_MAX] = {0};

/* W^X toggle counters for JIT debugging */
static _Atomic uint64_t wxcount_to_rx = 0;  /* RW→RX (exec fault) */
static _Atomic uint64_t wxcount_to_rw = 0;  /* RX→RW (write fault) */
static _Atomic uint64_t sysreg_write_count = 0;  /* EC=0x18 Dir=0 (DC CVAU, IC IVAU, etc.) */

/* Process table for tracking fork children */
static proc_entry_t proc_table[PROC_TABLE_SIZE];
static int64_t next_guest_pid = 2;
static pthread_mutex_t pid_lock = PTHREAD_MUTEX_INITIALIZER; /* Lock order: 6 */

/* Global flag for exit_group: signals all threads to terminate.
 * Atomic to avoid undefined behavior under C11 memory model when
 * multiple threads read/write concurrently. */
_Atomic int exit_group_requested = 0;

/* Exit code set by the thread that calls exit_group */
_Atomic int exit_group_code = 0;

/* ---------- Rosetta memory dump for debugging ---------- */

/* Dump guest memory regions to files for post-mortem analysis.
 * Called when rosetta's JIT assertion fires (BRK → SIG_DFL SIGTRAP).
 * Writes:
 *   /tmp/hl-dump-regs.txt   — vCPU register state
 *   /tmp/hl-dump-regions.txt — guest region listing
 *   /tmp/hl-dump-XXXX-XXXX.bin — binary dumps of each region
 */
static void dump_rosetta_state(hv_vcpu_t vcpu, guest_t *g) {
    fprintf(stderr, "hl: === ROSETTA STATE DUMP ===\n");

    /* 1. Dump all vCPU registers */
    FILE *rf = fopen("/tmp/hl-dump-regs.txt", "w");
    if (rf) {
        for (int i = 0; i <= 30; i++) {
            uint64_t v;
            hv_vcpu_get_reg(vcpu, (hv_reg_t)(HV_REG_X0 + i), &v);
            fprintf(rf, "X%-2d = 0x%016llx\n", i, (unsigned long long)v);
        }
        uint64_t sp, pc, cpsr;
        hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &sp);
        hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
        uint64_t elr;
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &elr);
        fprintf(rf, "SP  = 0x%016llx\n", (unsigned long long)sp);
        fprintf(rf, "PC  = 0x%016llx\n", (unsigned long long)pc);
        fprintf(rf, "ELR = 0x%016llx\n", (unsigned long long)elr);
        fprintf(rf, "CPSR= 0x%016llx\n", (unsigned long long)cpsr);
        fclose(rf);
        fprintf(stderr, "hl: dumped registers → /tmp/hl-dump-regs.txt\n");
    }

    /* 2. Dump region listing + binary data */
    FILE *lf = fopen("/tmp/hl-dump-regions.txt", "w");
    for (int i = 0; i < g->nregions; i++) {
        guest_region_t *r = &g->regions[i];
        uint64_t start = r->start;
        uint64_t end   = r->end;
        uint64_t size  = end - start;

        /* Resolve display VA for aliases */
        uint64_t disp_start = start;
        uint64_t disp_end   = end;
        if (r->display_va) {
            disp_start = r->display_va;
            disp_end   = r->display_va + size;
        } else {
            for (int j = 0; j < g->naliases; j++) {
                if (start >= g->va_aliases[j].gpa_start &&
                    end <= g->va_aliases[j].gpa_start + g->va_aliases[j].size) {
                    disp_start = g->va_aliases[j].va_start +
                                 (start - g->va_aliases[j].gpa_start);
                    disp_end = disp_start + size;
                    break;
                }
            }
        }

        char perms[5] = "----";
        if (r->prot & 1) perms[0] = 'r';
        if (r->prot & 2) perms[1] = 'w';
        if (r->prot & 4) perms[2] = 'x';
        perms[3] = (r->flags & 1) ? 's' : 'p';

        if (lf) {
            fprintf(lf, "%012llx-%012llx %s %s (gpa %012llx, %llu bytes)\n",
                    (unsigned long long)disp_start,
                    (unsigned long long)disp_end,
                    perms,
                    r->name[0] ? r->name : "<anon>",
                    (unsigned long long)start,
                    (unsigned long long)size);
        }

        /* Dump binary contents of interesting regions
         * (skip huge anonymous regions to avoid multi-GB dumps) */
        if (size > 0 && size <= 16 * 1024 * 1024 &&
            start < g->guest_size) {
            char path[256];
            snprintf(path, sizeof(path),
                     "/tmp/hl-dump-%012llx-%012llx.bin",
                     (unsigned long long)disp_start,
                     (unsigned long long)disp_end);
            FILE *bf = fopen(path, "wb");
            if (bf) {
                uint64_t safe_size = size;
                if (start + safe_size > g->guest_size)
                    safe_size = g->guest_size - start;
                fwrite((uint8_t *)g->host_base + start, 1, safe_size, bf);
                fclose(bf);
            }
        }
    }

    /* Also dump kbuf pages that were W^X toggled (rosetta JIT slab).
     * The kbuf sits at g->kbuf_gpa in the primary buffer. Dump the
     * first 16MB of it (covers rosetta's active JIT pages). */
    if (g->kbuf_base) {
        char path[256];
        uint64_t dump_size = 16 * 1024 * 1024;
        snprintf(path, sizeof(path), "/tmp/hl-dump-kbuf-16MB.bin");
        FILE *bf = fopen(path, "wb");
        if (bf) {
            fwrite(g->kbuf_base, 1, dump_size, bf);
            fclose(bf);
            if (lf) fprintf(lf, "kbuf: %012llx (host %p, 16MB dumped)\n",
                            (unsigned long long)g->kbuf_gpa, g->kbuf_base);
        }
    }

    if (lf) {
        fclose(lf);
        fprintf(stderr, "hl: dumped regions → /tmp/hl-dump-regions.txt\n");
    }

    fprintf(stderr, "hl: === END DUMP ===\n");
}

/* ---------- Public API ---------- */

void proc_init(void) {
    guest_pid = 1;
    parent_pid = 0;
    next_guest_pid = 2;
    memset(proc_table, 0, sizeof(proc_table));
    thread_init();
    futex_init();
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

const unsigned char *proc_get_shim_blob(void) {
    return shim_blob_ptr;
}

unsigned int proc_get_shim_size(void) {
    return shim_blob_size;
}

void proc_set_elf_path(const char *path) {
    if (path) {
        /* Resolve to absolute path if relative */
        if (path[0] == '/') {
            strncpy(elf_path, path, sizeof(elf_path) - 1);
        } else {
            char cwd[LINUX_PATH_MAX];
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

void proc_set_hl_path(const char *path) {
    if (path) {
        strncpy(hl_path, path, sizeof(hl_path) - 1);
        hl_path[sizeof(hl_path) - 1] = '\0';
    }
}

const char *proc_get_hl_path(void) {
    return hl_path[0] ? hl_path : NULL;
}

void proc_set_cmdline(int argc, const char **argv) {
    size_t off = 0;
    for (int i = 0; i < argc && off < sizeof(cmdline_buf) - 1; i++) {
        size_t len = strlen(argv[i]);
        if (off + len + 1 > sizeof(cmdline_buf)) break;
        memcpy(cmdline_buf + off, argv[i], len);
        off += len;
        cmdline_buf[off++] = '\0'; /* NUL separator between args */
    }
    cmdline_len = off;
}

const char *proc_get_cmdline(size_t *len_out) {
    if (cmdline_len == 0) return NULL;
    if (len_out) *len_out = cmdline_len;
    return cmdline_buf;
}

void proc_set_sysroot(const char *path) {
    if (path && path[0]) {
        strncpy(sysroot_path, path, sizeof(sysroot_path) - 1);
        sysroot_path[sizeof(sysroot_path) - 1] = '\0';
        /* Strip trailing slash for consistent path joining */
        size_t len = strlen(sysroot_path);
        while (len > 1 && sysroot_path[len - 1] == '/')
            sysroot_path[--len] = '\0';
    } else {
        sysroot_path[0] = '\0';
    }
}

const char *proc_get_sysroot(void) {
    return sysroot_path[0] ? sysroot_path : NULL;
}

void proc_set_identity(int64_t pid, int64_t ppid) {
    guest_pid = pid;
    parent_pid = ppid;
}

int64_t proc_alloc_pid(void) {
    pthread_mutex_lock(&pid_lock);
    int64_t pid = next_guest_pid++;
    pthread_mutex_unlock(&pid_lock);
    return pid;
}

/* Try to reap exited children from the process table. Calls waitpid
 * with WNOHANG on each active entry; entries whose host process has
 * exited are freed. Returns the number of slots reclaimed. */
static int proc_reap_finished(void) {
    int reaped = 0;
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (!proc_table[i].active) continue;
        if (proc_table[i].exited) {
            /* Already marked exited but never waited — free the slot */
            proc_table[i].active = 0;
            reaped++;
            continue;
        }
        int status;
        pid_t ret = waitpid(proc_table[i].host_pid, &status, WNOHANG);
        if (ret > 0) {
            /* Child exited — free the slot */
            proc_table[i].active = 0;
            reaped++;
        }
    }
    return reaped;
}

void proc_register_child(pid_t host_pid, int64_t guest_pid_val) {
    pthread_mutex_lock(&pid_lock);
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (!proc_table[i].active) {
            proc_table[i].active = 1;
            proc_table[i].host_pid = host_pid;
            proc_table[i].guest_pid = guest_pid_val;
            proc_table[i].exited = 0;
            proc_table[i].exit_status = 0;
            pthread_mutex_unlock(&pid_lock);
            return;
        }
    }

    /* Table full — try reaping exited children, then retry */
    if (proc_reap_finished() > 0) {
        for (int i = 0; i < PROC_TABLE_SIZE; i++) {
            if (!proc_table[i].active) {
                proc_table[i].active = 1;
                proc_table[i].host_pid = host_pid;
                proc_table[i].guest_pid = guest_pid_val;
                proc_table[i].exited = 0;
                proc_table[i].exit_status = 0;
                pthread_mutex_unlock(&pid_lock);
                return;
            }
        }
    }
    pthread_mutex_unlock(&pid_lock);

    fprintf(stderr, "hl: process table full (%d slots), child PID %lld dropped\n",
            PROC_TABLE_SIZE, (long long)guest_pid_val);
}

void proc_mark_child_exited(pid_t host_pid, int status) {
    pthread_mutex_lock(&pid_lock);
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (proc_table[i].active && proc_table[i].host_pid == host_pid) {
            proc_table[i].exited = 1;
            proc_table[i].exit_status = status;
            pthread_mutex_unlock(&pid_lock);
            return;
        }
    }
    pthread_mutex_unlock(&pid_lock);
}

/* ---------- sys_ptrace ---------- */

int64_t sys_ptrace(guest_t *g, uint64_t request, int64_t pid,
                   uint64_t addr, uint64_t data) {
    switch (request) {

    case LINUX_PTRACE_SEIZE: {
        /* Attach to target thread without stopping it. The tracee
         * can later be stopped via PTRACE_INTERRUPT or BRK-induced
         * ptrace-stop. Unlike PTRACE_ATTACH, SEIZE does not send
         * SIGSTOP — Rosetta relies on this. */
        thread_entry_t *target = thread_find(pid);
        if (!target)
            return -LINUX_ESRCH;
        if (target->ptraced)
            return -LINUX_EPERM;

        target->ptraced    = 1;
        target->tracer_tid = current_thread->guest_tid;
        return 0;
    }

    case LINUX_PTRACE_CONT: {
        /* Resume a stopped tracee, optionally injecting a signal.
         * data = signal to inject (0 = none). */
        thread_entry_t *target = thread_find(pid);
        if (!target || !target->ptraced)
            return -LINUX_ESRCH;
        if (!target->ptrace_stopped)
            return -LINUX_ESRCH;

        thread_ptrace_cont(target, (int)data);
        return 0;
    }

    case LINUX_PTRACE_INTERRUPT: {
        /* Force a running tracee into ptrace-stop. Uses hv_vcpus_exit
         * to break the tracee out of hv_vcpu_run; the tracee will then
         * enter ptrace-stop in its HV_EXIT_REASON_CANCELED handler. */
        thread_entry_t *target = thread_find(pid);
        if (!target || !target->ptraced)
            return -LINUX_ESRCH;
        if (target->ptrace_stopped)
            return 0;  /* Already stopped */

        hv_vcpus_exit(&target->vcpu, 1);
        return 0;
    }

    case LINUX_PTRACE_GETREGSET: {
        /* Read tracee registers via iovec. addr = NT_PRSTATUS (1),
         * data = guest pointer to linux iovec_t {base, len}. */
        if (addr != LINUX_NT_PRSTATUS)
            return -LINUX_EINVAL;

        thread_entry_t *target = thread_find(pid);
        if (!target || !target->ptraced || !target->ptrace_stopped)
            return -LINUX_ESRCH;

        /* Read guest iovec */
        linux_iovec_t iov;
        if (guest_read(g, data, &iov, sizeof(iov)) < 0)
            return -LINUX_EFAULT;

        /* Copy register data (truncate if iovec is smaller) */
        size_t copy_len = sizeof(linux_user_pt_regs_t);
        if (iov.iov_len < copy_len)
            copy_len = iov.iov_len;

        if (guest_write(g, iov.iov_base, &target->ptrace_regs, copy_len) < 0)
            return -LINUX_EFAULT;

        /* Write back actual bytes transferred */
        iov.iov_len = copy_len;
        if (guest_write(g, data, &iov, sizeof(iov)) < 0)
            return -LINUX_EFAULT;

        return 0;
    }

    case LINUX_PTRACE_SETREGSET: {
        /* Write tracee registers via iovec. addr = NT_PRSTATUS (1),
         * data = guest pointer to linux iovec_t {base, len}. */
        if (addr != LINUX_NT_PRSTATUS)
            return -LINUX_EINVAL;

        thread_entry_t *target = thread_find(pid);
        if (!target || !target->ptraced || !target->ptrace_stopped)
            return -LINUX_ESRCH;

        /* Read guest iovec */
        linux_iovec_t iov;
        if (guest_read(g, data, &iov, sizeof(iov)) < 0)
            return -LINUX_EFAULT;

        /* Copy register data from guest */
        size_t copy_len = sizeof(linux_user_pt_regs_t);
        if (iov.iov_len < copy_len)
            copy_len = iov.iov_len;

        if (guest_read(g, iov.iov_base, &target->ptrace_regs, copy_len) < 0)
            return -LINUX_EFAULT;

        target->ptrace_regs_dirty = 1;

        /* Write back actual bytes transferred */
        iov.iov_len = copy_len;
        if (guest_write(g, data, &iov, sizeof(iov)) < 0)
            return -LINUX_EFAULT;

        return 0;
    }

    default:
        return -LINUX_EINVAL;
    }
}

/* Write a macOS struct rusage to guest memory as linux_rusage_t.
 * The structs have identical field layout on LP64. */
static int write_rusage_to_guest(guest_t *g, uint64_t gva,
                                  const struct rusage *ru) {
    linux_rusage_t lru = {
        .ru_utime   = { ru->ru_utime.tv_sec,  ru->ru_utime.tv_usec },
        .ru_stime   = { ru->ru_stime.tv_sec,  ru->ru_stime.tv_usec },
        .ru_maxrss  = ru->ru_maxrss,
        .ru_ixrss   = ru->ru_ixrss,
        .ru_idrss   = ru->ru_idrss,
        .ru_isrss   = ru->ru_isrss,
        .ru_minflt  = ru->ru_minflt,
        .ru_majflt  = ru->ru_majflt,
        .ru_nswap   = ru->ru_nswap,
        .ru_inblock = ru->ru_inblock,
        .ru_oublock = ru->ru_oublock,
        .ru_msgsnd  = ru->ru_msgsnd,
        .ru_msgrcv  = ru->ru_msgrcv,
        .ru_nsignals = ru->ru_nsignals,
        .ru_nvcsw   = ru->ru_nvcsw,
        .ru_nivcsw  = ru->ru_nivcsw,
    };
    return guest_write(g, gva, &lru, sizeof(lru));
}

/* ---------- sys_wait4 ---------- */

int64_t sys_wait4(guest_t *g, int pid, uint64_t status_gva,
                  int options, uint64_t rusage_gva) {

    /* First check for ptraced or vm-clone children in the thread table.
     * Rosetta's two-process JIT uses clone(CLONE_VM) + ptrace, and the
     * child runs as an in-process thread, not a separate host process.
     * thread_ptrace_wait handles both ptrace-stopped and vm-exited states. */
    if (current_thread) {
        int ptrace_status = 0;
        int64_t ptrace_tid = thread_ptrace_wait(
            current_thread->guest_tid, pid, &ptrace_status, options);
        if (ptrace_tid > 0) {
            if (status_gva) {
                int32_t ls = ptrace_status;
                guest_write(g, status_gva, &ls, sizeof(ls));
            }
            return ptrace_tid;
        }
        /* ptrace_tid == 0: no matching children or WNOHANG — fall through
         * to the process table for regular fork children. */
    }

    /* Translate Linux wait options */
    int mac_options = 0;
    if (options & 1) mac_options |= WNOHANG;   /* WNOHANG = 1 on both */
    if (options & 2) mac_options |= WUNTRACED; /* WUNTRACED = 2 on both */

    pthread_mutex_lock(&pid_lock);

    if (pid == -1) {
        /* Wait for any child. Find any active entry in process table. */
        for (int i = 0; i < PROC_TABLE_SIZE; i++) {
            if (proc_table[i].active) {
                if (proc_table[i].exited) {
                    /* Already reaped (from CLONE_VFORK wait) */
                    int64_t gpid = proc_table[i].guest_pid;
                    int32_t linux_status = proc_table[i].exit_status;
                    proc_table[i].active = 0;
                    pthread_mutex_unlock(&pid_lock);
                    if (status_gva)
                        guest_write(g, status_gva, &linux_status, 4);
                    return gpid;
                }

                pid_t host_pid = proc_table[i].host_pid;
                int64_t gpid = proc_table[i].guest_pid;
                int slot = i;
                pthread_mutex_unlock(&pid_lock);

                int status;
                struct rusage ru;
                pid_t ret = wait4(host_pid, &status, mac_options,
                                  rusage_gva ? &ru : NULL);
                if (ret > 0) {
                    if (status_gva) {
                        int32_t linux_status = status;
                        guest_write(g, status_gva, &linux_status, 4);
                    }
                    if (rusage_gva)
                        write_rusage_to_guest(g, rusage_gva, &ru);
                    pthread_mutex_lock(&pid_lock);
                    /* Re-validate slot: another thread may have reaped it */
                    if (proc_table[slot].active &&
                        proc_table[slot].host_pid == host_pid)
                        proc_table[slot].active = 0;
                    pthread_mutex_unlock(&pid_lock);
                    return gpid;
                } else if (ret == 0) {
                    /* WNOHANG and child not yet exited */
                    return 0;
                }
                /* waitpid failed — try next child */
                pthread_mutex_lock(&pid_lock);
            }
        }
        pthread_mutex_unlock(&pid_lock);
        /* No children */
        return -LINUX_ECHILD;
    }

    /* Wait for specific guest PID */
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (proc_table[i].active && proc_table[i].guest_pid == pid) {
            if (proc_table[i].exited) {
                int64_t gpid = proc_table[i].guest_pid;
                int32_t linux_status = proc_table[i].exit_status;
                proc_table[i].active = 0;
                pthread_mutex_unlock(&pid_lock);
                if (status_gva)
                    guest_write(g, status_gva, &linux_status, 4);
                return gpid;
            }

            pid_t host_pid = proc_table[i].host_pid;
            int64_t gpid = proc_table[i].guest_pid;
            int slot = i;
            pthread_mutex_unlock(&pid_lock);

            int status;
            struct rusage ru;
            pid_t ret = wait4(host_pid, &status, mac_options,
                              rusage_gva ? &ru : NULL);
            if (ret > 0) {
                if (status_gva) {
                    int32_t linux_status = status;
                    guest_write(g, status_gva, &linux_status, 4);
                }
                if (rusage_gva)
                    write_rusage_to_guest(g, rusage_gva, &ru);
                pthread_mutex_lock(&pid_lock);
                /* Re-validate slot: another thread may have reaped it */
                if (proc_table[slot].active &&
                    proc_table[slot].host_pid == host_pid)
                    proc_table[slot].active = 0;
                pthread_mutex_unlock(&pid_lock);
                /* Queue SIGCHLD for parent process */
                signal_queue(LINUX_SIGCHLD);
                return gpid;
            } else if (ret == 0) {
                return 0; /* WNOHANG */
            }
            return linux_errno();
        }
    }
    pthread_mutex_unlock(&pid_lock);

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
    pthread_mutex_lock(&pid_lock);
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (!proc_table[i].active) continue;

        /* Match: P_ALL matches any, P_PID matches guest_pid */
        if (idtype == P_PID && proc_table[i].guest_pid != wait_pid)
            continue;

        int status;
        pid_t ret;
        int32_t gpid32 = (int32_t)proc_table[i].guest_pid;

        if (proc_table[i].exited) {
            /* Already reaped (from CLONE_VFORK wait) */
            status = proc_table[i].exit_status;
            ret = proc_table[i].host_pid;
        } else {
            pid_t host_pid = proc_table[i].host_pid;
            pthread_mutex_unlock(&pid_lock);
            ret = waitpid(host_pid, &status, mac_options);
            if (ret == 0) return 0; /* WNOHANG, child not yet exited */
            if (ret < 0) return linux_errno();
            pthread_mutex_lock(&pid_lock);
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
            memcpy(si + SIGINFO_OFF_PID, &gpid32, 4);
            uint32_t uid = 0;
            memcpy(si + SIGINFO_OFF_UID, &uid, 4);
            memcpy(si + SIGINFO_OFF_STATUS, &si_status, 4);

            guest_write(g, infop_gva, si, SIGINFO_SIZE);
        }

        /* Don't remove from table if WNOWAIT is set */
        if (!(options & 0x01000000))
            proc_table[i].active = 0;

        pthread_mutex_unlock(&pid_lock);
        return 0; /* waitid returns 0 on success */
    }
    pthread_mutex_unlock(&pid_lock);

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

/* Unified vCPU execution loop for both main and worker threads.
 *
 * When timeout_sec > 0 (main thread): uses alarm() for per-iteration
 * safety timeout, logs with "hl:" prefix.
 *
 * When timeout_sec == 0 (worker thread): skips alarm() setup (SIGALRM
 * is process-wide and would conflict). Workers are terminated by
 * exit_group setting exit_group_requested and calling hv_vcpus_exit()
 * to cancel pending hv_vcpu_run calls. Logs with "hl: worker" prefix.
 *
 * Both modes check exit_group_requested so the main thread also reacts
 * to exit_group called by a worker. */
int vcpu_run_loop(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit,
                  guest_t *g, int verbose, int timeout_sec) {
    int exit_code = 0;
    int running = 1;
    int iter = 0;
    const int is_main = (timeout_sec > 0);
    const char *prefix = is_main ? "hl" : "hl: worker";

    /* Main thread: set up alarm-based per-iteration timeout.
     * Guest ITIMER_REAL is emulated internally by signal_check_timer()
     * rather than using host setitimer, because macOS shares alarm()
     * and setitimer(ITIMER_REAL) as the same underlying timer. */
    if (is_main) {
        g_timeout_vcpu = vcpu;
        g_timed_out = 0;
        signal(SIGALRM, alarm_handler);
    }

    while (running) {
        /* Check if another thread called exit_group */
        if (atomic_load(&exit_group_requested)) {
            exit_code = atomic_load(&exit_group_code);
            break;
        }

        if (verbose) {
            uint64_t pc;
            hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
            fprintf(stderr, "%s: [%d] vcpu_run PC=0x%llx\n",
                    prefix, iter, (unsigned long long)pc);
        }
        iter++;

        /* Main: arm per-iteration safety timeout */
        if (is_main)
            alarm((unsigned)timeout_sec);

        HV_CHECK_CTX(hv_vcpu_run(vcpu), vcpu, g);

        /* Main: disarm timeout */
        if (is_main)
            alarm(0);

        /* Re-check exit_group after waking from hv_vcpu_run */
        if (atomic_load(&exit_group_requested)) {
            exit_code = atomic_load(&exit_group_code);
            break;
        }

        /* Main: check for alarm timeout */
        if (is_main && g_timed_out) {
            fprintf(stderr, "%s: vCPU execution timed out after %ds\n",
                    prefix, timeout_sec);

            uint64_t pc, cpsr;
            hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
            hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
            fprintf(stderr, "%s: timeout state: PC=0x%llx CPSR=0x%llx\n",
                    prefix,
                    (unsigned long long)pc, (unsigned long long)cpsr);

            uint64_t esr, far_reg, elr, sctlr_val;
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &esr);
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &far_reg);
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &elr);
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr_val);
            fprintf(stderr, "%s: ESR_EL1=0x%llx FAR_EL1=0x%llx "
                    "ELR_EL1=0x%llx SCTLR_EL1=0x%llx\n",
                    prefix,
                    (unsigned long long)esr,
                    (unsigned long long)far_reg,
                    (unsigned long long)elr,
                    (unsigned long long)sctlr_val);

            crash_report(vcpu, g, CRASH_TIMEOUT, NULL);
            exit_code = 124;
            break;
        }

        if (vexit->reason == HV_EXIT_REASON_EXCEPTION) {
            uint32_t ec = (vexit->exception.syndrome >> 26) & 0x3F;

            if (ec == 0x16) {
                /* HVC exit */
                uint16_t imm = vexit->exception.syndrome & 0xFFFF;

                if (verbose)
                    fprintf(stderr, "%s: HVC #%u\n", prefix, imm);

                switch (imm) {
                case 5: {
                    /* HVC #5: Linux syscall forwarding */

                    /* Track rosetta TLS JIT gate flag. Bit 0 of
                     * [X18+0x1b8] must be set for the SIGTRAP handler
                     * to perform JIT retranslation. Log when it changes. */
                    if (verbose && g->is_rosetta) {
                        static uint32_t prev_jit_gate = 0xDEADBEEF;
                        static uint64_t prev_x18 = 0;
                        uint64_t x18_val;
                        hv_vcpu_get_reg(vcpu, HV_REG_X18, &x18_val);
                        if (x18_val != prev_x18) {
                            fprintf(stderr, "%s: X18 changed: 0x%llx → 0x%llx "
                                    "(iter %d)\n", prefix,
                                    (unsigned long long)prev_x18,
                                    (unsigned long long)x18_val, iter);
                            prev_x18 = x18_val;
                            prev_jit_gate = 0xDEADBEEF; /* force re-check */
                        }
                        if (x18_val) {
                            uint32_t jit_gate;
                            if (guest_read(g, x18_val + 0x1b8, &jit_gate, 4) == 0 &&
                                jit_gate != prev_jit_gate) {
                                fprintf(stderr, "%s: JIT gate [0x%llx+0x1b8] "
                                        "changed: 0x%x → 0x%x (iter %d)\n",
                                        prefix, (unsigned long long)x18_val,
                                        prev_jit_gate, jit_gate, iter);
                                prev_jit_gate = jit_gate;
                            }
                        }
                    }

                    int ret = syscall_dispatch(vcpu, g, &exit_code, verbose);
                    if (ret == 1)
                        running = 0;
                    /* ret == SYSCALL_EXEC_HAPPENED: exec replaced the process,
                     * X0 already set by sys_execve, just continue loop */

                    /* Check guest ITIMER_REAL expiry (queues SIGALRM if due) */
                    signal_check_timer();

                    /* Diagnostic: log signal state after exec/sigreturn
                     * to help debug signal delivery issues. */
                    if (ret == SYSCALL_EXEC_HAPPENED && verbose) {
                        const signal_state_t *ss = signal_get_state();
                        uint64_t tblocked = current_thread ?
                            current_thread->blocked : 0xDEAD;
                        fprintf(stderr, "%s: post-sigreturn state: "
                                "pending=0x%llx global_blocked=0x%llx "
                                "thread_blocked=0x%llx "
                                "signal_pending=%d\n", prefix,
                                (unsigned long long)ss->pending,
                                (unsigned long long)ss->blocked,
                                (unsigned long long)tblocked,
                                signal_pending());
                    }

                    /* Deliver pending signals after each syscall */
                    if (running && signal_pending()) {
                        int sig_ret = signal_deliver(vcpu, g, &exit_code);
                        if (sig_ret < 0)
                            running = 0; /* Default TERM/CORE disposition */
                    }

                    /* After exec, verify critical registers before resuming
                     * vCPU. This closes any gap where signal delivery or
                     * other code between sys_execve's sync flush and
                     * hv_vcpu_run could have modified ELR_EL1. */
                    if (running && ret == SYSCALL_EXEC_HAPPENED) {
                        uint64_t verify_elr;
                        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1,
                                            &verify_elr);
                        if (verify_elr == 0) {
                            fprintf(stderr, "%s: FATAL: ELR_EL1=0 after "
                                    "exec, register sync failed\n", prefix);
                            crash_report(vcpu, g, CRASH_ELR_ZERO,
                                         "ELR_EL1=0 after exec");
                            exit_code = 128;
                            running = 0;
                        }
                    }
                    break;
                }

                case 0: {
                    /* HVC #0: Normal exit */
                    uint64_t x0;
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
                    if (verbose)
                        fprintf(stderr, "%s: guest exit HVC #0 code=%llu\n",
                                prefix, (unsigned long long)x0);
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
                        fprintf(stderr, "%s: HVC #4 unknown reg %llu\n",
                                prefix, (unsigned long long)reg_id);
                        exit_code = 128;
                        running = 0;
                        continue;
                    }
                    if (verbose)
                        fprintf(stderr, "%s: HVC #4 set reg %llu = 0x%llx\n",
                                prefix,
                                (unsigned long long)reg_id,
                                (unsigned long long)value);
                    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, hv_reg, value));
                    break;
                }

                case 7: {
                    /* HVC #7: MRS trap emulation — guest EL0 code read
                     * a system register (e.g., ID_AA64MMFR1_EL1 for
                     * feature detection). Extract the register encoding
                     * from ESR_EL1's ISS field and read it via HVF.
                     * Return value in X0 for the shim to store into the
                     * saved register frame. */
                    uint64_t esr;
                    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &esr);
                    uint32_t iss = (uint32_t)(esr & 0x1FFFFFF);

                    /* ISS encoding for EC=0x18 (MSR/MRS trap):
                     *   [21:20] = Op0    [19:17] = Op2
                     *   [16:14] = Op1    [13:10] = CRn
                     *   [9:5]   = Rt     [4:1]   = CRm
                     *   [0]     = Direction (1=MRS read) */
                    uint32_t op0 = (iss >> 20) & 0x3;
                    uint32_t op2 = (iss >> 17) & 0x7;
                    uint32_t op1 = (iss >> 14) & 0x7;
                    uint32_t crn = (iss >> 10) & 0xF;
                    uint32_t crm = (iss >> 1)  & 0xF;

                    /* Construct HVF system register ID:
                     *   (Op0<<14) | (Op1<<11) | (CRn<<7) | (CRm<<3) | Op2 */
                    hv_sys_reg_t reg = (hv_sys_reg_t)(
                        (op0 << 14) | (op1 << 11) |
                        (crn << 7)  | (crm << 3)  | op2);

                    uint64_t value = 0;

                    /* ID register emulation: return VZ-sanitized values
                     * matching a real VZ (Lima) VM BEFORE trying HVF.
                     * HVF's hv_vcpu_get_sys_reg succeeds for ID registers
                     * but returns raw hardware values — which include
                     * features the hypervisor doesn't actually virtualize.
                     * Rosetta's JIT uses these to select code generation
                     * paths; exposing unsupported features causes wrong
                     * JIT code and assertion failures.
                     *
                     * Values captured from a Lima VZ VM on Apple Silicon
                     * via inline MRS from EL0 (kernel trap-and-emulate).
                     * These are checked first, before the HVF call. */
                    int have_vz_override = 0;

                    /* ID_AA64MMFR0_EL1 (3,0,0,7,0) */
                    if (op0 == 3 && op1 == 0 && crn == 0 &&
                        crm == 7 && op2 == 0) {
                        value = 0x00000111ff000000ULL;
                        have_vz_override = 1;
                    }
                    /* ID_AA64MMFR1_EL1 (3,0,0,7,1) — VZ returns 0.
                     * Raw hardware (e.g., 0x11212000) exposes HPDS, PAN,
                     * LO, XNX etc. that VZ doesn't virtualize. */
                    if (op0 == 3 && op1 == 0 && crn == 0 &&
                        crm == 7 && op2 == 1) {
                        value = 0x0000000000000000ULL;
                        have_vz_override = 1;
                    }
                    /* ID_AA64MMFR2_EL1 (3,0,0,7,2) — VZ returns 0. */
                    if (op0 == 3 && op1 == 0 && crn == 0 &&
                        crm == 7 && op2 == 2) {
                        value = 0x0000000000000000ULL;
                        have_vz_override = 1;
                    }
                    /* ID_AA64ISAR0_EL1 (3,0,0,6,0) */
                    if (op0 == 3 && op1 == 0 && crn == 0 &&
                        crm == 6 && op2 == 0) {
                        value = 0x0021100110212120ULL;
                        have_vz_override = 1;
                    }
                    /* ID_AA64ISAR1_EL1 (3,0,0,6,1) */
                    if (op0 == 3 && op1 == 0 && crn == 0 &&
                        crm == 6 && op2 == 1) {
                        value = 0x0000101110211402ULL;
                        have_vz_override = 1;
                    }
                    /* ID_AA64PFR0_EL1 (3,0,0,4,0) */
                    if (op0 == 3 && op1 == 0 && crn == 0 &&
                        crm == 4 && op2 == 0) {
                        value = 0x0001000000110011ULL;
                        have_vz_override = 1;
                    }
                    /* ID_AA64PFR1_EL1 (3,0,0,4,1) — VZ returns 0. */
                    if (op0 == 3 && op1 == 0 && crn == 0 &&
                        crm == 4 && op2 == 1) {
                        value = 0x0000000000000000ULL;
                        have_vz_override = 1;
                    }

                    if (have_vz_override) {
                        if (verbose)
                            fprintf(stderr, "%s: MRS trap: Op0=%u Op1=%u "
                                    "CRn=%u CRm=%u Op2=%u → 0x%llx (VZ)\n",
                                    prefix, op0, op1, crn, crm, op2,
                                    (unsigned long long)value);
                    }

                    hv_return_t ret = have_vz_override ? HV_SUCCESS
                        : hv_vcpu_get_sys_reg(vcpu, reg, &value);
                    if (ret != HV_SUCCESS) {
                        /* HVF doesn't expose this register. Provide a
                         * host-side fallback for known registers. */
                        int have_fallback = 0;

                        /* CNTFRQ_EL0 (3,3,14,0,0): counter frequency.
                         * Read directly from host hardware — Apple Silicon
                         * uses 24MHz. Rosetta needs this for timing. */
                        if (op0 == 3 && op1 == 3 && crn == 14 &&
                            crm == 0 && op2 == 0) {
                            __asm__ volatile("mrs %0, cntfrq_el0"
                                             : "=r"(value));
                            have_fallback = 1;
                        }

                        /* Non-ID register fallbacks for registers
                         * that HVF doesn't expose. ID registers are
                         * handled above (VZ overrides). */

                        if (verbose) {
                            if (have_fallback) {
                                fprintf(stderr, "%s: MRS trap: "
                                        "Op0=%u Op1=%u CRn=%u CRm=%u "
                                        "Op2=%u → 0x%llx (host)\n",
                                        prefix, op0, op1, crn, crm, op2,
                                        (unsigned long long)value);
                            } else {
                                fprintf(stderr, "%s: MRS trap: unknown reg "
                                        "Op0=%u Op1=%u CRn=%u CRm=%u "
                                        "Op2=%u (hv_reg=0x%x) → 0\n",
                                        prefix, op0, op1, crn, crm, op2,
                                        (unsigned)reg);
                            }
                        }
                    } else if (verbose) {
                        fprintf(stderr, "%s: MRS trap: Op0=%u Op1=%u "
                                "CRn=%u CRm=%u Op2=%u → 0x%llx\n",
                                prefix, op0, op1, crn, crm, op2,
                                (unsigned long long)value);
                    }

                    /* ID_AA64MMFR1_EL1 (3,0,0,7,1): pass through real
                     * hardware value without modification. Real Lima VZ
                     * VMs return the actual hardware value (no FEAT_AFP
                     * on M1/M2), and rosetta runs successfully with
                     * quality=1 JIT. Faking FEAT_AFP to unlock quality=2
                     * combined with other hl-specific behaviors causes
                     * "BasicBlock requested for unrecognized address"
                     * assertion failures. */

                    hv_vcpu_set_reg(vcpu, HV_REG_X0, value);
                    break;
                }

                case 9: {
                    /* HVC #9: W^X page permission toggle for JIT.
                     *
                     * Apple HVF enforces W^X: pages cannot be both writable
                     * and executable simultaneously. JIT translators (rosetta)
                     * need to write code (RW), then execute it (RX). The shim
                     * detects permission faults (EC=0x20 instruction abort,
                     * EC=0x24 data abort) and forwards the faulting address
                     * here.
                     *
                     * Toggling at 2MB granularity causes thrashing when the
                     * JIT writes new code and executes existing code within
                     * the same 2MB block. Instead, we split the 2MB block
                     * into 4KB L3 pages and toggle only the faulting 4KB page.
                     * This allows different pages within a 2MB block to have
                     * independent RW/RX permissions simultaneously.
                     *
                     * x0 = FAR_EL1 (faulting virtual address)
                     * x1 = type: 0 = exec fault → flip to RX
                     *            1 = write fault → flip to RW */
                    uint64_t far, type;
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &far);
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &type);

                    uint64_t page_start = far & ~(4096ULL - 1);
                    uint64_t page_end = page_start + 4096;
                    int new_perms = (type == 0) ? MEM_PERM_RX : MEM_PERM_RW;

                    /* Count W^X toggles for JIT debugging */
                    if (type == 0)
                        atomic_fetch_add(&wxcount_to_rx, 1);
                    else
                        atomic_fetch_add(&wxcount_to_rw, 1);

                    if (verbose)
                        fprintf(stderr, "%s: W^X toggle at 0x%llx → %s "
                                "(page 0x%llx)\n",
                                prefix, (unsigned long long)far,
                                (type == 0) ? "RX" : "RW",
                                (unsigned long long)page_start);

                    /* Route to TTBR1 handler for kernel VA addresses
                     * (rosetta's JIT cache, internal allocations) or
                     * TTBR0 handler for normal user VA addresses. */
                    if (far >= KBUF_VA_BASE && g->kbuf_base) {
                        int sr = guest_kbuf_split_block(g,
                                     far & ~(BLOCK_2MB - 1));
                        int ur = guest_kbuf_update_perms(g, page_start,
                                     page_end, new_perms);
                        if (verbose && (sr < 0 || ur < 0))
                            fprintf(stderr, "%s: W^X kbuf toggle FAILED "
                                    "(split=%d update=%d)\n",
                                    prefix, sr, ur);
                    } else {
                        uint64_t block_start = far & ~(BLOCK_2MB - 1);
                        int sr = guest_split_block(g, block_start);
                        int ur = guest_update_perms(g, page_start,
                                     page_end, new_perms);
                        if (verbose && (sr < 0 || ur < 0))
                            fprintf(stderr, "%s: W^X user toggle FAILED "
                                    "(split=%d update=%d) far=0x%llx\n",
                                    prefix, sr, ur,
                                    (unsigned long long)far);
                    }
                    /* TLB flush is done by the shim (tlbi_restore_eret) */
                    g->need_tlbi = 0;
                    break;
                }

                case 10: {
                    /* HVC #10: BRK from EL0 → deliver SIGTRAP or ptrace-stop.
                     *
                     * Rosetta's JIT uses BRK instructions as trampolines
                     * and internal breakpoints. If the thread is ptraced,
                     * the BRK enters a ptrace-stop (the tracer reads/writes
                     * registers then CONT's). Otherwise we queue SIGTRAP
                     * and deliver it via the signal frame mechanism.
                     *
                     * The shim has already restored all GPRs to their EL0
                     * values, so signal_deliver / ptrace_stop read correct state.
                     *
                     * The Linux kernel sets si_code=TRAP_BRKPT, si_addr=BRK_PC,
                     * and fault_address=BRK_PC for BRK-triggered SIGTRAP.
                     * Rosetta's handler needs these to identify the trap type
                     * and resume execution correctly. */
                    uint64_t brk_pc;
                    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &brk_pc);

                    if (verbose) {
                        fprintf(stderr, "%s: BRK at 0x%llx → %s\n",
                                prefix, (unsigned long long)brk_pc,
                                current_thread->ptraced ? "ptrace-stop" : "SIGTRAP");
                    }

                    if (current_thread->ptraced) {
                        /* Ptrace-stop: suspend vCPU, notify tracer.
                         * thread_ptrace_stop blocks until tracer CONT's. */
                        int cont_sig = thread_ptrace_stop(current_thread, 5);
                        if (cont_sig > 0) {
                            signal_queue(cont_sig);
                            int sr = signal_deliver(vcpu, g, &exit_code);
                            if (sr < 0) running = 0;
                        }
                    } else {
                        /* Non-ptraced: deliver SIGTRAP via signal frame.
                         * Read ESR_EL1 to include in sigcontext — rosetta's
                         * handler reads the BRK immediate from the ESR to
                         * determine the trap type (#3=AOT, #7=indirect, etc). */
                        uint64_t brk_esr;
                        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &brk_esr);
                        signal_set_fault_info(LINUX_TRAP_BRKPT, brk_pc, brk_esr);
                        signal_queue(5);  /* SIGTRAP = 5 on aarch64-linux */
                        if (verbose) {
                            uint64_t thread_blocked = current_thread ?
                                current_thread->blocked : 0xDEAD;
                            fprintf(stderr, "%s: BRK: thread_blocked=0x%llx "
                                    "pending=0x%llx\n",
                                    prefix,
                                    (unsigned long long)thread_blocked,
                                    (unsigned long long)signal_get_state()->pending);
                        }
                        int sig_ret = signal_deliver(vcpu, g, &exit_code);
                        if (verbose)
                            fprintf(stderr, "%s: signal_deliver returned %d\n",
                                    prefix, sig_ret);
                        if (sig_ret < 0) {
                            /* SIG_DFL for SIGTRAP: rosetta assertion crash.
                             * Dump all memory state for post-mortem analysis
                             * of the JIT/AOT translation failure. */
                            dump_rosetta_state(vcpu, g);
                            running = 0;
                        }
                    }
                    break;
                }

                case 11: {
                    /* HVC #11: EL0 fault → deliver SIGSEGV.
                     *
                     * The shim forwards translation faults, access flag
                     * faults, and read permission faults from EL0 here.
                     * A real Linux kernel delivers SIGSEGV for these.
                     * This enables fault-driven lazy JIT translation:
                     * rosetta registers a SIGSEGV handler and uses faults
                     * to discover untranslated code addresses.
                     *
                     * Decode fault type from ESR_EL1:
                     *   EC=0x20 (instruction abort) or EC=0x24 (data abort)
                     *   xFSC[5:2]: 0x01=translation, 0x03=permission
                     *   WnR (bit 6): 1=write, 0=read (data abort only)
                     *
                     * si_code mapping:
                     *   Translation fault → SEGV_MAPERR (address not mapped)
                     *   Permission fault  → SEGV_ACCERR (bad permissions) */
                    uint64_t esr, far_addr;
                    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &esr);
                    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &far_addr);

                    uint32_t ec = (uint32_t)((esr >> 26) & 0x3F);
                    uint32_t fsc = (uint32_t)(esr & 0x3F);
                    uint32_t fsc_type = (fsc >> 2) & 0xF;  /* xFSC[5:2] */

                    /* Determine si_code based on fault type */
                    int si_code;
                    if (fsc_type == 0x03) {
                        si_code = LINUX_SEGV_ACCERR;  /* Permission fault */
                    } else {
                        si_code = LINUX_SEGV_MAPERR;  /* Translation/other */
                    }

                    if (verbose) {
                        const char *fault_type =
                            (ec == 0x20) ? "inst" : "data";
                        const char *code_name =
                            (si_code == LINUX_SEGV_MAPERR) ? "MAPERR" : "ACCERR";
                        fprintf(stderr, "%s: EL0 %s fault at 0x%llx "
                                "(ESR=0x%llx FSC=0x%x) → SIGSEGV/%s\n",
                                prefix, fault_type,
                                (unsigned long long)far_addr,
                                (unsigned long long)esr,
                                fsc, code_name);
                    }

                    signal_set_fault_info(si_code, far_addr, esr);
                    signal_queue(LINUX_SIGSEGV);
                    int sig_ret = signal_deliver(vcpu, g, &exit_code);
                    if (verbose)
                        fprintf(stderr, "%s: SIGSEGV deliver returned %d\n",
                                prefix, sig_ret);
                    if (sig_ret < 0) {
                        /* Default action: core dump (we just terminate) */
                        running = 0;
                    }
                    break;
                }

                case 12: {
                    /* HVC #12: System instruction trap (EC=0x18 Direction=0).
                     * The shim forwards trapped cache maintenance instructions
                     * (DC CVAU, IC IVAU, etc.) here for logging/counting.
                     * The shim has already executed IC IALLU and advanced PC. */
                    atomic_fetch_add(&sysreg_write_count, 1);
                    if (verbose) {
                        uint64_t esr;
                        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &esr);
                        uint32_t iss = (uint32_t)(esr & 0x1FFFFFF);
                        /* Decode ISS for system instruction:
                         *   Op0[21:20] Op2[19:17] Op1[16:14]
                         *   CRn[13:10] Rt[9:5] CRm[4:1] Dir[0] */
                        uint32_t op0 = (iss >> 20) & 0x3;
                        uint32_t op2 = (iss >> 17) & 0x7;
                        uint32_t op1 = (iss >> 14) & 0x7;
                        uint32_t crn = (iss >> 10) & 0xF;
                        uint32_t crm = (iss >> 1)  & 0xF;
                        uint32_t rt  = (iss >> 5)  & 0x1F;
                        /* DC CVAU: Op0=1,Op1=3,CRn=7,CRm=11,Op2=1
                         * IC IVAU: Op0=1,Op1=3,CRn=7,CRm=5,Op2=1 */
                        const char *name = "unknown";
                        if (op0==1 && op1==3 && crn==7 && crm==11 && op2==1)
                            name = "DC CVAU";
                        else if (op0==1 && op1==3 && crn==7 && crm==5 && op2==1)
                            name = "IC IVAU";
                        else if (op0==1 && op1==3 && crn==7 && crm==10 && op2==1)
                            name = "DC CVAC";
                        else if (op0==1 && op1==3 && crn==7 && crm==14 && op2==1)
                            name = "DC CIVAC";
                        fprintf(stderr, "%s: sysreg trap #%llu: %s "
                                "(Op0=%u Op1=%u CRn=%u CRm=%u Op2=%u Rt=X%u)\n",
                                prefix,
                                (unsigned long long)atomic_load(&sysreg_write_count),
                                name, op0, op1, crn, crm, op2, rt);
                    }
                    break;
                }

                case 2: {
                    /* HVC #2: Bad exception in guest.
                     * Shim clobbers X0-X3,X5 with exception info.
                     * X4,X6-X30 and SP_EL0 still hold faulting values. */
                    uint64_t x0, x1, x2, x3, x5;
                    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
                    hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1);
                    hv_vcpu_get_reg(vcpu, HV_REG_X2, &x2);
                    hv_vcpu_get_reg(vcpu, HV_REG_X3, &x3);
                    hv_vcpu_get_reg(vcpu, HV_REG_X5, &x5);
                    fprintf(stderr, "%s: guest exception vec=0x%03llx "
                            "ESR=0x%llx FAR=0x%llx ELR=0x%llx SPSR=0x%llx\n",
                            prefix,
                            (unsigned long long)x5,
                            (unsigned long long)x0,
                            (unsigned long long)x1,
                            (unsigned long long)x2,
                            (unsigned long long)x3);

                    /* Dump preserved registers for debugging */
                    uint64_t sp_el0;
                    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &sp_el0);
                    fprintf(stderr, "%s:   SP_EL0=0x%llx\n", prefix,
                            (unsigned long long)sp_el0);
                    for (int ri = 4; ri <= 30; ri++) {
                        /* Skip X5 (clobbered by shim for vec offset) */
                        if (ri >= 0 && ri <= 3) continue;
                        if (ri == 5) continue;
                        uint64_t rv;
                        hv_vcpu_get_reg(vcpu,
                                        (hv_reg_t)(HV_REG_X0 + ri), &rv);
                        fprintf(stderr, "%s:   X%-2d=0x%016llx\n", prefix,
                                ri, (unsigned long long)rv);
                    }

                    /* Try to read faulting instruction from kbuf
                     * (for JIT code at kernel VA) */
                    uint64_t elr = x2;  /* ELR_EL1 */
                    if (elr >= KBUF_VA_BASE && g->kbuf_base) {
                        uint64_t koff = elr - KBUF_VA_BASE;
                        if (koff + 4 <= KBUF_SIZE) {
                            uint32_t insn;
                            memcpy(&insn,
                                   (uint8_t *)g->kbuf_base + koff, 4);
                            fprintf(stderr, "%s:   insn@ELR=0x%08x\n",
                                    prefix, insn);
                        }
                    }

                    /* Check if FAR looks like a tagged pointer */
                    uint64_t far = x1;
                    uint16_t top16 = (uint16_t)(far >> 48);
                    if (top16 != 0x0000 && top16 != 0xFFFF) {
                        fprintf(stderr, "%s:   FAR tag=0x%04x, "
                                "extracted addr=0x%llx\n",
                                prefix, top16,
                                (unsigned long long)(far & 0x0000FFFFFFFFFFFFULL));
                    }

                    {
                        char detail[128];
                        snprintf(detail, sizeof(detail),
                                 "vec=0x%03llx ESR=0x%llx FAR=0x%llx",
                                 (unsigned long long)x5,
                                 (unsigned long long)x0,
                                 (unsigned long long)x1);
                        crash_report(vcpu, g, CRASH_BAD_EXCEPTION, detail);
                    }
                    exit_code = 128;
                    running = 0;
                    break;
                }

                default: {
                    fprintf(stderr, "%s: unexpected HVC #%u\n", prefix, imm);
                    char detail[64];
                    snprintf(detail, sizeof(detail), "HVC #%u", imm);
                    crash_report(vcpu, g, CRASH_UNEXPECTED_HVC, detail);
                    exit_code = 128;
                    running = 0;
                    break;
                }
                }
            } else if (ec == 0x01) {
                /* WFI/WFE trapped — just continue */
                if (verbose)
                    fprintf(stderr, "%s: WFI/WFE trapped\n", prefix);
            } else {
                /* Non-HVC exception at EL2 level */
                fprintf(stderr, "%s: unexpected exception EC=0x%x "
                        "syndrome=0x%llx VA=0x%llx PA=0x%llx\n",
                        prefix, ec,
                        (unsigned long long)vexit->exception.syndrome,
                        (unsigned long long)vexit->exception.virtual_address,
                        (unsigned long long)vexit->exception.physical_address);
                {
                    char detail[128];
                    snprintf(detail, sizeof(detail),
                             "EC=0x%x syndrome=0x%llx VA=0x%llx",
                             ec,
                             (unsigned long long)vexit->exception.syndrome,
                             (unsigned long long)vexit->exception.virtual_address);
                    crash_report(vcpu, g, CRASH_UNEXPECTED_EC, detail);
                }
                exit_code = 128;
                running = 0;
            }
        } else if (vexit->reason == HV_EXIT_REASON_CANCELED) {
            /* Canceled by hv_vcpus_exit(). Can be: alarm timeout,
             * exit_group from another thread, or signal preemption
             * (signal_queue called hv_vcpus_exit to deliver a signal
             * while the guest was in a tight loop). */
            if (is_main && g_timed_out) {
                /* Timeout already handled above the exception switch —
                 * loop back so the timeout check fires. */
                continue;
            }
            if (atomic_load(&exit_group_requested)) {
                exit_code = atomic_load(&exit_group_code);
                running = 0;
                break;
            }

            /* PTRACE_INTERRUPT: if this thread is ptraced and not already
             * stopped, enter ptrace-stop so the tracer can inspect state.
             * This handles hv_vcpus_exit from sys_ptrace PTRACE_INTERRUPT. */
            if (current_thread->ptraced && !current_thread->ptrace_stopped) {
                if (verbose)
                    fprintf(stderr, "%s: ptrace interrupt → ptrace-stop\n",
                            prefix);
                int cont_sig = thread_ptrace_stop(current_thread, 5);
                if (cont_sig > 0) {
                    signal_queue(cont_sig);
                    int sr = signal_deliver(vcpu, g, &exit_code);
                    if (sr < 0) running = 0;
                }
                continue;
            }

            /* Check guest ITIMER_REAL (may have fired during tight loop) */
            signal_check_timer();

            /* Signal preemption: if a signal is pending, deliver it and
             * resume the vCPU. This enables alarm()/SIGALRM delivery
             * and tgkill-based signals in compute-bound loops. */
            if (signal_pending()) {
                int sig_ret = signal_deliver(vcpu, g, &exit_code);
                if (sig_ret < 0)
                    running = 0;
                /* sig_ret >= 0: signal delivered or nothing pending,
                 * loop back and resume vCPU execution */
                continue;
            }

            /* No signal pending — truly unexpected cancelation */
            if (verbose)
                fprintf(stderr, "%s: vCPU canceled (no signal pending)\n",
                        prefix);
        } else {
            fprintf(stderr, "%s: unexpected exit reason 0x%x\n",
                    prefix, vexit->reason);
            {
                char detail[64];
                snprintf(detail, sizeof(detail),
                         "exit reason 0x%x", vexit->reason);
                crash_report(vcpu, g, CRASH_UNEXPECTED_EXIT, detail);
            }
            exit_code = 128;
            running = 0;
        }
    }

    /* Clean up timeout if we set it up */
    if (is_main) {
        signal(SIGALRM, SIG_DFL);
        alarm(0);
    }

    return exit_code;
}
