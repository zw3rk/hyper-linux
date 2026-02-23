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

/* Sysroot path for dynamic linker library resolution */
static char sysroot_path[LINUX_PATH_MAX] = {0};

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
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (!proc_table[i].active) {
            proc_table[i].active = 1;
            proc_table[i].host_pid = host_pid;
            proc_table[i].guest_pid = guest_pid_val;
            proc_table[i].exited = 0;
            proc_table[i].exit_status = 0;
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
                return;
            }
        }
    }

    fprintf(stderr, "hl: process table full (%d slots), child PID %lld dropped\n",
            PROC_TABLE_SIZE, (long long)guest_pid_val);
}

void proc_mark_child_exited(pid_t host_pid, int status) {
    for (int i = 0; i < PROC_TABLE_SIZE; i++) {
        if (proc_table[i].active && proc_table[i].host_pid == host_pid) {
            proc_table[i].exited = 1;
            proc_table[i].exit_status = status;
            return;
        }
    }
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

        HV_CHECK(hv_vcpu_run(vcpu));

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
                    int ret = syscall_dispatch(vcpu, g, &exit_code, verbose);
                    if (ret == 1)
                        running = 0;
                    /* ret == SYSCALL_EXEC_HAPPENED: exec replaced the process,
                     * X0 already set by sys_execve, just continue loop */

                    /* Check guest ITIMER_REAL expiry (queues SIGALRM if due) */
                    signal_check_timer();

                    /* Diagnostic: log signal state after exec to help
                     * debug ELR_EL1 corruption from stale handlers. */
                    if (ret == SYSCALL_EXEC_HAPPENED && verbose) {
                        const signal_state_t *ss = signal_get_state();
                        fprintf(stderr, "%s: post-exec signal state: "
                                "pending=0x%llx blocked=0x%llx\n", prefix,
                                (unsigned long long)ss->pending,
                                (unsigned long long)ss->blocked);
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

                case 2: {
                    /* HVC #2: Bad exception in guest */
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
                    exit_code = 128;
                    running = 0;
                    break;
                }

                default:
                    fprintf(stderr, "%s: unexpected HVC #%u\n", prefix, imm);
                    exit_code = 128;
                    running = 0;
                    break;
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
