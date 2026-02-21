/* syscall_proc.h — Process state and management for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Owns all static process state (PIDs, shim blob, ELF path, cmdline,
 * process table). Provides accessor functions so other modules
 * (fork_ipc, syscall_exec, proc_emulation) can interact with this
 * state without direct access.
 *
 * Also contains wait4/waitid and the vCPU run loop.
 */
#ifndef SYSCALL_PROC_H
#define SYSCALL_PROC_H

#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include <sys/types.h>
#include "guest.h"
#include "elf.h"

/* ---------- Process state ---------- */

/* Initialize the process subsystem. */
void proc_init(void);

/* Get/set current guest PID and PPID. */
int64_t proc_get_pid(void);
int64_t proc_get_ppid(void);

/* Store shim blob pointer/size (called from hl.c at startup).
 * Avoids #including shim_blob.h in this module. */
void proc_set_shim(const unsigned char *blob, unsigned int len);

/* Get shim blob pointer (for exec and fork IPC). */
const unsigned char *proc_get_shim_blob(void);
unsigned int proc_get_shim_size(void);

/* Store the current ELF binary path for /proc/self/exe emulation.
 * Called from hl.c at startup and after execve. */
void proc_set_elf_path(const char *path);

/* Get the stored ELF binary path. Returns NULL if not set. */
const char *proc_get_elf_path(void);

/* Store the guest command line for /proc/self/cmdline emulation.
 * argv is a NULL-terminated array of strings. */
void proc_set_cmdline(int argc, const char **argv);

/* Get the stored cmdline buffer and its length. Returns NULL if not set. */
const char *proc_get_cmdline(size_t *len_out);

/* Set guest identity (called from fork_child_main). */
void proc_set_identity(int64_t pid, int64_t ppid);

/* Allocate next guest PID (called from sys_clone). */
int64_t proc_alloc_pid(void);

/* Store the sysroot path for dynamic linker library resolution.
 * When set, absolute library paths (e.g. /lib/libc.so) are prefixed
 * with this path. Pass NULL to clear. */
void proc_set_sysroot(const char *path);

/* Get the stored sysroot path. Returns NULL if not set. */
const char *proc_get_sysroot(void);

/* ---------- execve ---------- */

/* Return value from syscall_dispatch: 2 means "exec happened, skip X0 write" */
#define SYSCALL_EXEC_HAPPENED 2

/* ---------- Process table (for fork/clone children) ---------- */

#define PROC_TABLE_SIZE 64

typedef struct {
    int     active;     /* 1 if slot is in use */
    pid_t   host_pid;   /* macOS process ID of child hl instance */
    int64_t guest_pid;  /* Guest-visible PID assigned to child */
    int     exited;     /* 1 if child has exited */
    int     exit_status;/* wait status (as returned by waitpid) */
} proc_entry_t;

/* Register a child process in the process table. */
void proc_register_child(pid_t host_pid, int64_t guest_pid);

/* Mark a child as exited by host PID (for CLONE_VFORK wait). */
void proc_mark_child_exited(pid_t host_pid, int status);

/* ---------- wait ---------- */

/* Wait for child process. Returns child guest PID or negative errno. */
int64_t sys_wait4(guest_t *g, int pid, uint64_t status_gva,
                  int options, uint64_t rusage_gva);

/* waitid: wait for child process using idtype/id semantics.
 * Fills siginfo_t at infop_gva on success. */
int64_t sys_waitid(guest_t *g, int idtype, int64_t id,
                   uint64_t infop_gva, int options);

/* ---------- exit_group coordination ---------- */

/* Global flag for exit_group: signals all threads to terminate.
 * Set by the thread calling SYS_exit_group; checked by worker
 * threads in their run loop to break out after hv_vcpu_run.
 * Using volatile int instead of sig_atomic_t to avoid pulling in
 * <signal.h> which defines sa_handler as a macro (conflicts with
 * our linux_sigaction_t struct field). */
extern volatile int exit_group_requested;

/* Exit code set by the thread that calls exit_group, so workers
 * can return the same code. */
extern int exit_group_code;

/* ---------- vCPU run loop ---------- */

/* Run the vCPU execution loop. Returns the exit code.
 * This is extracted from hl.c main() so both normal execution
 * and fork-child mode can share the same loop logic.
 * When is_worker is non-zero, alarm() timeout is skipped (workers
 * are terminated by exit_group via hv_vcpus_exit instead). */
int vcpu_run_loop(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit,
                  guest_t *g, int verbose, int timeout_sec);

/* Worker-thread variant of vcpu_run_loop. Same as above but skips
 * alarm() setup and checks exit_group_requested after each iteration. */
int vcpu_run_loop_worker(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit,
                         guest_t *g, int verbose);

#endif /* SYSCALL_PROC_H */
