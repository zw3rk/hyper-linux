/* thread.h — Per-thread state for Linux threading support in hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Maintains a table of guest threads. Each thread has its own HVF vCPU
 * running on a dedicated host pthread. The main thread is registered at
 * startup; worker threads are added via clone(CLONE_THREAD). A
 * _Thread_local pointer provides O(1) access to the current thread's
 * entry from any syscall handler.
 *
 * SP_EL1 allocation: each thread gets a 4KB EL1 exception stack carved
 * from the shim data region (SHIM_DATA_BASE + 2MB). Thread 0 (main)
 * gets the top, thread N gets offset -(N * 4096).
 */
#ifndef THREAD_H
#define THREAD_H

#include <Hypervisor/Hypervisor.h>
#include <pthread.h>
#include <stdint.h>
#include "syscall.h"  /* linux_user_pt_regs_t */

/* Maximum number of concurrent guest threads in one VM. */
#define MAX_THREADS 64

/* Per-thread state. One entry per guest thread (main + workers). */
typedef struct {
    int64_t       guest_tid;       /* Linux TID (unique per thread) */
    hv_vcpu_t     vcpu;            /* HVF vCPU handle for this thread */
    hv_vcpu_exit_t *vexit;         /* vCPU exit info pointer */
    pthread_t     host_thread;     /* macOS host thread running this vCPU */
    uint64_t      clear_child_tid; /* GVA for CLONE_CHILD_CLEARTID (0=none) */
    uint64_t      sp_el1;          /* Per-thread EL1 stack top (IPA) */
    int           active;          /* Non-zero while thread is running */
    /* Per-thread signal mask (POSIX requires each thread to have its own).
     * Initialized to the parent's mask on clone, modified via rt_sigprocmask. */
    uint64_t      blocked;         /* Signal mask for this thread */
    uint64_t      saved_blocked;   /* Original mask saved by sigsuspend */
    int           saved_blocked_valid;

    /* Per-thread alternate signal stack (Linux sigaltstack is per-thread).
     * Fields mirror linux_stack_t layout for easy copy. */
    uint64_t      altstack_sp;     /* Alternate signal stack pointer */
    int32_t       altstack_flags;  /* SS_DISABLE / 0 */
    uint64_t      altstack_size;   /* Alternate signal stack size */
    int           on_altstack;     /* Non-zero if currently delivering on altstack */

    /* ---------- ptrace state ----------
     * Used by Rosetta's two-process JIT: the tracer attaches via
     * PTRACE_SEIZE, then uses BRK-triggered SIGTRAP + wait4 to
     * discover untranslated code on-demand. The tracee snapshots
     * its own vCPU registers before stopping and applies any
     * tracer-written changes on resume — this avoids cross-thread
     * HVF register access (which may not be supported). */
    int             ptraced;          /* Non-zero if being traced */
    int64_t         tracer_tid;       /* TID of tracing thread */
    int             ptrace_stopped;   /* Non-zero when in ptrace-stop */
    int             ptrace_stop_sig;  /* Signal that caused the stop */
    pthread_cond_t  ptrace_cond;      /* Tracee stopped → tracer wakes */
    pthread_cond_t  resume_cond;      /* Tracer CONT → tracee wakes */
    int             ptrace_cont_sig;  /* Signal to inject on resume (0=none) */
    linux_user_pt_regs_t ptrace_regs; /* Register snapshot for cross-thread access */
    int             ptrace_regs_dirty;/* Tracer modified registers */

    /* ---------- VM-clone child state ----------
     * For clone(CLONE_VM) without CLONE_THREAD: shares guest memory but
     * has a separate TID, is waitable via wait4, and can be ptraced.
     * Used by Rosetta's two-process JIT architecture. */
    int             is_vm_clone;      /* Waitable via wait4 */
    int64_t         parent_tid;       /* Parent TID for wait4 matching */
    int             exit_signal;      /* Signal on exit (usually SIGCHLD) */
    int             vm_exited;        /* Child has exited */
    int             vm_exit_status;   /* Wait-format exit status */
} thread_entry_t;

/* Current thread pointer — set once per host pthread at thread start.
 * All syscall handlers can access per-thread state through this. */
extern _Thread_local thread_entry_t *current_thread;

/* Initialize the thread table. Call once before any thread operations. */
void thread_init(void);

/* Register the main thread (thread 0). Called from hl.c after the
 * initial vCPU is created. Sets current_thread for the main thread. */
void thread_register_main(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit,
                           int64_t tid, uint64_t sp_el1);

/* Allocate a new thread table slot for the given TID.
 * Returns a pointer to the entry, or NULL if the table is full.
 * The caller must fill in vcpu, vexit, host_thread, sp_el1. */
thread_entry_t *thread_alloc(int64_t tid);

/* Mark a thread as inactive and release its table slot. */
void thread_deactivate(thread_entry_t *t);

/* Find a thread by guest TID. Returns NULL if not found. */
thread_entry_t *thread_find(int64_t tid);

/* Count currently active threads. */
int thread_active_count(void);

/* Allocate a per-thread SP_EL1 value. Thread N gets the Nth 4KB slot
 * counting down from the top of the shim data region. The IPA base
 * (GUEST_IPA_BASE + SHIM_DATA_BASE + 2MB) is the main thread's SP_EL1;
 * each subsequent thread subtracts 4KB. Returns the IPA, or 0 on failure. */
uint64_t thread_alloc_sp_el1(void);

/* Iterate over all active threads, calling fn(entry, ctx) for each.
 * Holds the thread table lock during iteration. */
void thread_for_each(void (*fn)(thread_entry_t *t, void *ctx), void *ctx);

/* Count active VM-clone threads (is_vm_clone && !vm_exited).
 * Used to detect when the last rosetta tracee exits. */
int thread_count_active_vm_clones(void);

/* Destroy all active worker vCPUs. Called during guest_destroy to
 * ensure no vCPUs remain active before hv_vm_destroy(). */
void thread_destroy_all_vcpus(void);

/* Interrupt all active vCPUs by calling hv_vcpus_exit().
 * Used for signal preemption: when a signal is queued while a vCPU
 * is running in a tight loop (no syscalls), this forces it to break
 * out of hv_vcpu_run so the signal can be delivered. */
void thread_interrupt_all(void);

/* ---------- Ptrace helpers ---------- */

/* Initialize ptrace-related fields (conds, zero state). Called from
 * thread_alloc() and thread_register_main(). */
void thread_ptrace_init(thread_entry_t *t);

/* Tracee: snapshot vCPU regs, enter ptrace-stop, block until resumed.
 * Returns the signal to inject (from tracer's PTRACE_CONT), or 0. */
int thread_ptrace_stop(thread_entry_t *t, int sig);

/* Tracer: resume a stopped tracee with optional signal injection. */
void thread_ptrace_cont(thread_entry_t *t, int sig);

/* Tracer: wait for a ptraced or vm-clone child to stop or exit.
 * Returns child TID on success, 0 on WNOHANG with none ready,
 * or negative Linux errno. Writes wait-format status to *out_status. */
int64_t thread_ptrace_wait(int64_t tracer_tid, int pid, int *out_status,
                            int options);

/* Get the thread table mutex (needed for ptrace wait blocking). */
pthread_mutex_t *thread_get_lock(void);

#endif /* THREAD_H */
