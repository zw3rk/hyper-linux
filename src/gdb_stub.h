/* gdb_stub.h — GDB Remote Serial Protocol stub for guest debugging
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements a GDB RSP stub over TCP so that aarch64-linux-gnu-gdb or
 * LLDB can connect and debug the guest ELF binary running inside hl.
 * Uses hardware breakpoints (DBGBVR/DBGBCR) and watchpoints (DBGWVR/
 * DBGWCR) via hv_vcpu_set_trap_debug_exceptions(). All-stop mode.
 *
 * Supported:
 *   - aarch64 guests only (not x86_64/rosetta — JIT makes it meaningless)
 *   - Hardware breakpoints (Z1/z1, up to 16)
 *   - Hardware watchpoints (Z2/z2 write, Z3/z3 read, Z4/z4 access, up to 16)
 *   - Single-step via temporary hardware breakpoint at next PC
 *   - All GPR + SIMD/FP register read/write
 *   - Memory read/write
 *   - Multi-thread support (qfThreadInfo, Hg, vCont)
 *   - qXfer:features:read:target.xml for register layout
 */
#ifndef GDB_STUB_H
#define GDB_STUB_H

#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include "guest.h"

/* Stop reasons reported to GDB */
#define GDB_STOP_NONE       0
#define GDB_STOP_BREAKPOINT 1
#define GDB_STOP_WATCHPOINT 2
#define GDB_STOP_STEP       3
#define GDB_STOP_SIGNAL     4
#define GDB_STOP_ENTRY      5  /* stop-on-entry before first instruction */

/* Initialize the GDB stub. Starts a TCP listener on the given port.
 * Returns 0 on success, -1 on failure. Must be called before the
 * vCPU run loop. The stub thread runs in the background and processes
 * GDB packets asynchronously. */
int gdb_stub_init(int port, guest_t *g);

/* Block until a GDB client connects and sends its initial packet.
 * Call this after gdb_stub_init() when --gdb-stop-on-entry is set
 * to halt the guest before the first instruction executes. */
void gdb_stub_wait_for_attach(void);

/* Check if the GDB stub is active (initialized and client connected). */
int gdb_stub_is_active(void);

/* Notify the GDB stub that a debug exception occurred on the current
 * thread's vCPU. The vCPU is already stopped (hv_vcpu_run returned).
 * This blocks the calling thread until GDB resumes it.
 *
 * ec: exception class from syndrome (0x30=hw bp, 0x32=step, 0x34=wp)
 * Returns: 0 to continue, 1 to single-step next instruction. */
int gdb_stub_handle_stop(int stop_reason, uint64_t stop_addr);

/* Check if GDB has requested all vCPUs to stop (Ctrl+C or breakpoint
 * on another thread). Called from the vCPU loop after HV_EXIT_REASON_CANCELED.
 * Returns non-zero if this thread should enter gdb_stub_handle_stop. */
int gdb_stub_stop_requested(void);

/* Notify the stub of thread creation/exit for thread list tracking. */
void gdb_stub_notify_thread_created(int64_t tid);
void gdb_stub_notify_thread_exited(int64_t tid);

/* Program hardware debug registers for a specific vCPU.
 * Called when a thread is about to resume execution. This writes
 * the current breakpoint/watchpoint state to the vCPU's debug regs. */
void gdb_stub_sync_debug_regs(hv_vcpu_t vcpu);

/* Cleanup the GDB stub (close sockets, stop listener thread). */
void gdb_stub_shutdown(void);

#endif /* GDB_STUB_H */
