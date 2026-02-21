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

#endif /* THREAD_H */
