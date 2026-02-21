/* thread.c — Per-thread state for Linux threading support in hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Thread table with _Thread_local fast path for current thread access.
 * Protected by a mutex since thread creation/destruction is infrequent
 * relative to per-syscall access (which uses the lock-free TLS pointer).
 */
#include "thread.h"
#include "guest.h"   /* SHIM_DATA_BASE, BLOCK_2MB, GUEST_IPA_BASE */

#include <stdio.h>
#include <string.h>

/* ---------- Thread table ---------- */

static thread_entry_t thread_table[MAX_THREADS];
static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;

/* Track the next SP_EL1 slot index. Slot 0 is the main thread (top of
 * shim data region); each subsequent thread gets the next 4KB down. */
static int next_sp_el1_slot = 0;

/* Per-thread pointer to the current thread's entry. Set once when a
 * host pthread starts running a guest vCPU. Syscall handlers read this
 * without locking — it's thread-local and never changes after init. */
_Thread_local thread_entry_t *current_thread = NULL;

/* ---------- Public API ---------- */

void thread_init(void) {
    pthread_mutex_lock(&thread_lock);
    memset(thread_table, 0, sizeof(thread_table));
    next_sp_el1_slot = 0;
    current_thread = NULL;
    pthread_mutex_unlock(&thread_lock);
}

void thread_register_main(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit,
                           int64_t tid, uint64_t sp_el1) {
    pthread_mutex_lock(&thread_lock);

    thread_entry_t *t = &thread_table[0];
    t->guest_tid       = tid;
    t->vcpu            = vcpu;
    t->vexit           = vexit;
    t->host_thread     = pthread_self();
    t->clear_child_tid = 0;
    t->sp_el1          = sp_el1;
    t->active          = 1;

    /* Slot 0 is consumed by main thread */
    next_sp_el1_slot = 1;

    pthread_mutex_unlock(&thread_lock);

    /* Set TLS pointer for the main thread */
    current_thread = t;
}

thread_entry_t *thread_alloc(int64_t tid) {
    thread_entry_t *result = NULL;

    pthread_mutex_lock(&thread_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!thread_table[i].active) {
            thread_entry_t *t = &thread_table[i];
            memset(t, 0, sizeof(*t));
            t->guest_tid = tid;
            t->active    = 1;
            result = t;
            break;
        }
    }
    pthread_mutex_unlock(&thread_lock);

    return result;
}

void thread_deactivate(thread_entry_t *t) {
    if (!t) return;

    pthread_mutex_lock(&thread_lock);
    t->active = 0;
    pthread_mutex_unlock(&thread_lock);
}

thread_entry_t *thread_find(int64_t tid) {
    thread_entry_t *result = NULL;

    pthread_mutex_lock(&thread_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].active && thread_table[i].guest_tid == tid) {
            result = &thread_table[i];
            break;
        }
    }
    pthread_mutex_unlock(&thread_lock);

    return result;
}

int thread_active_count(void) {
    int count = 0;

    pthread_mutex_lock(&thread_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].active) count++;
    }
    pthread_mutex_unlock(&thread_lock);

    return count;
}

uint64_t thread_alloc_sp_el1(void) {
    uint64_t sp = 0;

    pthread_mutex_lock(&thread_lock);

    if (next_sp_el1_slot >= MAX_THREADS) {
        fprintf(stderr, "thread: SP_EL1 slots exhausted\n");
    } else {
        /* Main thread's SP_EL1 = IPA_BASE + SHIM_DATA_BASE + 2MB.
         * Each subsequent thread is 4KB below. */
        uint64_t top = GUEST_IPA_BASE + SHIM_DATA_BASE + BLOCK_2MB;
        sp = top - (uint64_t)next_sp_el1_slot * 4096;
        next_sp_el1_slot++;
    }

    pthread_mutex_unlock(&thread_lock);

    return sp;
}

void thread_for_each(void (*fn)(thread_entry_t *t, void *ctx), void *ctx) {
    pthread_mutex_lock(&thread_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].active) {
            fn(&thread_table[i], ctx);
        }
    }
    pthread_mutex_unlock(&thread_lock);
}
