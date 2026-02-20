/* fork_ipc.h — Fork/clone IPC for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements clone via posix_spawn + IPC state transfer. macOS HVF allows
 * only one VM per process, so fork spawns a new hl process and serializes
 * the full VM state (registers, memory, FDs) over a socketpair.
 */
#ifndef FORK_IPC_H
#define FORK_IPC_H

#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include "guest.h"

/* Fork child entry point: receives VM state over IPC socket, creates VM,
 * enters vCPU run loop. Called from hl.c when --fork-child is specified.
 * Returns the process exit code. */
int fork_child_main(int ipc_fd, int verbose, int timeout_sec);

/* Clone syscall: spawn a new host hl process with IPC state transfer.
 * Returns child guest PID to parent, or negative Linux errno. */
int64_t sys_clone(hv_vcpu_t vcpu, guest_t *g, uint64_t flags,
                  uint64_t child_stack, uint64_t ptid_gva,
                  uint64_t tls, uint64_t ctid_gva, int verbose);

#endif /* FORK_IPC_H */
