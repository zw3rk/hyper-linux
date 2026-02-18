/* syscall_proc.h — Process management for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Handles process-related syscalls: getpid/getppid, execve, clone/fork,
 * wait4/waitid, and the process table for tracking children.
 */
#ifndef SYSCALL_PROC_H
#define SYSCALL_PROC_H

#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
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

/* ---------- execve ---------- */

/* Return value from syscall_dispatch: 2 means "exec happened, skip X0 write" */
#define SYSCALL_EXEC_HAPPENED 2

/* Execute a new binary, replacing current process image.
 * Reads path, argv[], envp[] from guest memory, reloads ELF, resets state.
 * Returns SYSCALL_EXEC_HAPPENED on success (caller skips X0 write),
 * or negative Linux errno on failure. */
int64_t sys_execve(hv_vcpu_t vcpu, guest_t *g,
                   uint64_t path_gva, uint64_t argv_gva, uint64_t envp_gva,
                   int verbose);

/* ---------- Process table (for fork/clone children) ---------- */

#define PROC_TABLE_SIZE 64

typedef struct {
    int     active;     /* 1 if slot is in use */
    pid_t   host_pid;   /* macOS process ID of child hl instance */
    int64_t guest_pid;  /* Guest-visible PID assigned to child */
    int     exited;     /* 1 if child has exited */
    int     exit_status;/* wait status (as returned by waitpid) */
} proc_entry_t;

/* ---------- fork/clone ---------- */

/* Fork child entry point: receives VM state over IPC socket, creates VM,
 * enters vCPU run loop. Called from hl.c when --fork-child is specified.
 * Returns the process exit code. */
int fork_child_main(int ipc_fd, int verbose, int timeout_sec);

/* Clone syscall: spawn a new host hl process with IPC state transfer.
 * Returns child guest PID to parent, or negative Linux errno. */
int64_t sys_clone(hv_vcpu_t vcpu, guest_t *g, uint64_t flags,
                  uint64_t child_stack, uint64_t ptid_gva,
                  uint64_t tls, uint64_t ctid_gva, int verbose);

/* Wait for child process. Returns child guest PID or negative errno. */
int64_t sys_wait4(guest_t *g, int pid, uint64_t status_gva,
                  int options, uint64_t rusage_gva);

/* ---------- vCPU run loop ---------- */

/* Run the vCPU execution loop. Returns the exit code.
 * This is extracted from hl.c main() so both normal execution
 * and fork-child mode can share the same loop logic. */
int vcpu_run_loop(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit,
                  guest_t *g, int verbose, int timeout_sec);

#endif /* SYSCALL_PROC_H */
