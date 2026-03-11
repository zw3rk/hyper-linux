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
#include "hv_util.h" /* vcpu_get_gpr, vcpu_get_sysreg */

/* From syscall_signal.h — included here directly to avoid pulling in
 * the full signal header (macOS defines sa_handler as a macro that
 * conflicts with our linux_sigaction_t field name). */
#define LINUX_SS_DISABLE 2

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---------- Thread table ---------- */

static thread_entry_t thread_table[MAX_THREADS];
static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER; /* Lock order: 5 */

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
    t->altstack_flags  = LINUX_SS_DISABLE;
    t->on_altstack     = 0;
    thread_ptrace_init(t);

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
            t->altstack_flags = 2; /* LINUX_SS_DISABLE */
            thread_ptrace_init(t);
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

    /* If this is a VM-clone child, mark it as exited and wake the
     * tracer/parent so wait4 can collect the exit status.  Must happen
     * BEFORE destroying the condvars — broadcasting a destroyed condvar
     * is undefined behavior. */
    /* Guard against double-deactivation: if already inactive, skip. */
    if (!t->active) {
        pthread_mutex_unlock(&thread_lock);
        return;
    }

    if (t->is_vm_clone) {
        t->vm_exited = 1;
        pthread_cond_broadcast(&t->ptrace_cond);
    }

    t->active = 0;

    /* Do NOT destroy ptrace condvars here — a waiter woken by the
     * broadcast above may still be inside pthread_cond_wait() trying
     * to re-acquire thread_lock. Destroying the condvar while a waiter
     * references it is undefined behavior. thread_ptrace_init() re-inits
     * condvars when the slot is reused via thread_alloc(). */

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

int thread_tid_alive(int64_t tid) {
    /* Lock-free scan: `active` transitions 1→0 exactly once (in
     * thread_deactivate under thread_lock), and `guest_tid` is set
     * at allocation and never changes until the slot is reused
     * (after active=0). A stale read of active=1 for a thread
     * being deactivated is benign — the caller retries on the
     * next poll iteration (100ms later). */
    for (int i = 0; i < MAX_THREADS; i++) {
        if (__atomic_load_n(&thread_table[i].active, __ATOMIC_ACQUIRE) &&
            thread_table[i].guest_tid == tid)
            return 1;
    }
    return 0;
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

int thread_count_active_vm_clones(void) {
    int count = 0;

    pthread_mutex_lock(&thread_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].active &&
            thread_table[i].is_vm_clone &&
            !thread_table[i].vm_exited)
            count++;
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

void thread_join_workers(void) {
    /* Snapshot worker threads under the lock. We need the host_thread
     * handle and a way to check the active flag without re-locking.
     * Storing the table index lets us use __atomic_load on active. */
    struct { pthread_t thr; int idx; } workers[MAX_THREADS];
    int nworkers = 0;

    pthread_mutex_lock(&thread_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].active && &thread_table[i] != current_thread) {
            workers[nworkers].thr = thread_table[i].host_thread;
            workers[nworkers].idx = i;
            nworkers++;
        }
    }
    pthread_mutex_unlock(&thread_lock);

    /* Poll/join each worker OUTSIDE the lock. Workers that responded
     * to hv_vcpus_exit typically finish within microseconds. Threads
     * stuck in uninterruptible host calls are abandoned after ~50ms. */
    for (int w = 0; w < nworkers; w++) {
        for (int i = 0; i < 10; i++) {
            if (!__atomic_load_n(&thread_table[workers[w].idx].active,
                                  __ATOMIC_ACQUIRE))
                break;
            usleep(5000);
        }

        if (!__atomic_load_n(&thread_table[workers[w].idx].active,
                              __ATOMIC_ACQUIRE))
            pthread_join(workers[w].thr, NULL);
        else
            pthread_detach(workers[w].thr);
    }
}

void thread_destroy_all_vcpus(void) {
    pthread_mutex_lock(&thread_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].active && thread_table[i].vcpu) {
            hv_vcpu_destroy(thread_table[i].vcpu);
            thread_table[i].vcpu = 0;
            thread_table[i].active = 0;
            /* Do NOT destroy condvars — same race as thread_deactivate:
             * a waiter woken by an earlier broadcast may still reference
             * the condvar. Process is exiting, so the leak is harmless. */
        }
    }
    pthread_mutex_unlock(&thread_lock);
}

void thread_interrupt_all(void) {
    /* Collect active vCPUs under the lock, then call hv_vcpus_exit
     * outside the lock to avoid holding it during a framework call. */
    hv_vcpu_t vcpus[MAX_THREADS];
    int count = 0;

    pthread_mutex_lock(&thread_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].active)
            vcpus[count++] = thread_table[i].vcpu;
    }
    pthread_mutex_unlock(&thread_lock);

    /* Force all active vCPUs out of hv_vcpu_run(). Each vCPU will see
     * HV_EXIT_REASON_CANCELED and check for pending signals. */
    if (count > 0)
        hv_vcpus_exit(vcpus, (uint32_t)count);
}

/* ---------- Ptrace helpers ---------- */

pthread_mutex_t *thread_get_lock(void) {
    return &thread_lock;
}

void thread_ptrace_init(thread_entry_t *t) {
    t->ptraced           = 0;
    t->tracer_tid        = 0;
    t->ptrace_stopped    = 0;
    t->ptrace_stop_sig   = 0;
    t->ptrace_cont_sig   = 0;
    t->ptrace_regs_dirty = 0;
    t->is_vm_clone       = 0;
    t->parent_tid        = 0;
    t->exit_signal       = 0;
    t->vm_exited         = 0;
    t->vm_exit_status    = 0;
    memset(&t->ptrace_regs, 0, sizeof(t->ptrace_regs));
    pthread_cond_init(&t->ptrace_cond, NULL);
    pthread_cond_init(&t->resume_cond, NULL);
}

int thread_ptrace_stop(thread_entry_t *t, int sig) {
    pthread_mutex_lock(&thread_lock);

    /* Snapshot vCPU registers into ptrace_regs so the tracer can read
     * them without cross-thread HVF access. */
    for (int i = 0; i < 31; i++)
        t->ptrace_regs.regs[i] = vcpu_get_gpr(t->vcpu, (unsigned)i);
    t->ptrace_regs.sp     = vcpu_get_sysreg(t->vcpu, HV_SYS_REG_SP_EL0);
    t->ptrace_regs.pc     = vcpu_get_sysreg(t->vcpu, HV_SYS_REG_ELR_EL1);
    t->ptrace_regs.pstate = vcpu_get_sysreg(t->vcpu, HV_SYS_REG_SPSR_EL1);

    /* Enter ptrace-stop state */
    t->ptrace_stopped   = 1;
    t->ptrace_stop_sig  = sig;
    t->ptrace_cont_sig  = 0;
    t->ptrace_regs_dirty = 0;

    /* Wake the tracer (blocked in thread_ptrace_wait) */
    pthread_cond_broadcast(&t->ptrace_cond);

    /* Block until tracer calls PTRACE_CONT */
    while (t->ptrace_stopped)
        pthread_cond_wait(&t->resume_cond, &thread_lock);

    /* Apply register changes if tracer wrote via SETREGSET */
    if (t->ptrace_regs_dirty) {
        for (int i = 0; i < 31; i++)
            vcpu_set_gpr(t->vcpu, (unsigned)i, t->ptrace_regs.regs[i]);
        vcpu_set_sysreg(t->vcpu, HV_SYS_REG_SP_EL0,   t->ptrace_regs.sp);
        vcpu_set_sysreg(t->vcpu, HV_SYS_REG_ELR_EL1,  t->ptrace_regs.pc);
        vcpu_set_sysreg(t->vcpu, HV_SYS_REG_SPSR_EL1, t->ptrace_regs.pstate);
        t->ptrace_regs_dirty = 0;
    }

    int cont_sig = t->ptrace_cont_sig;
    pthread_mutex_unlock(&thread_lock);

    return cont_sig;
}

void thread_ptrace_cont(thread_entry_t *t, int sig) {
    pthread_mutex_lock(&thread_lock);
    t->ptrace_cont_sig = sig;
    t->ptrace_stopped  = 0;
    pthread_cond_signal(&t->resume_cond);
    pthread_mutex_unlock(&thread_lock);
}

int64_t thread_ptrace_wait(int64_t tracer_tid, int pid, int *out_status,
                            int options) {
    int wnohang = (options & 1); /* WNOHANG = 1 on Linux */

    pthread_mutex_lock(&thread_lock);

    for (;;) {
        int found_any = 0;  /* Any waitable children at all? */

        for (int i = 0; i < MAX_THREADS; i++) {
            thread_entry_t *t = &thread_table[i];
            if (!t->active) continue;

            /* Match: ptraced children of this tracer, or vm-clone children */
            int is_child = 0;
            if (t->ptraced && t->tracer_tid == tracer_tid)
                is_child = 1;
            if (t->is_vm_clone && t->parent_tid == tracer_tid)
                is_child = 1;
            if (!is_child) continue;

            /* If pid > 0, match only specific TID */
            if (pid > 0 && t->guest_tid != pid) continue;

            found_any = 1;

            /* Check if ptrace-stopped */
            if (t->ptrace_stopped) {
                int64_t tid = t->guest_tid;
                if (out_status)
                    *out_status = (t->ptrace_stop_sig << 8) | 0x7F;
                pthread_mutex_unlock(&thread_lock);
                return tid;
            }

            /* Check if vm-clone child exited */
            if (t->vm_exited) {
                int64_t tid = t->guest_tid;
                if (out_status)
                    *out_status = t->vm_exit_status;
                /* Deactivate the slot and clean up condvars.
                 * vm-clone children do NOT call thread_deactivate() on exit
                 * (they skip it since the slot is collected here), so we
                 * must destroy condvars to avoid resource leaks. */
                t->active = 0;
                pthread_cond_destroy(&t->ptrace_cond);
                pthread_cond_destroy(&t->resume_cond);
                pthread_mutex_unlock(&thread_lock);
                return tid;
            }
        }

        if (!found_any) {
            pthread_mutex_unlock(&thread_lock);
            return 0;  /* No matching children — let caller fall through */
        }

        if (wnohang) {
            pthread_mutex_unlock(&thread_lock);
            return 0;
        }

        /* Block until some child's ptrace_cond fires.
         * We wait on the first matching child's cond — for simplicity,
         * scan again to find one. In practice Rosetta has one tracee. */
        for (int i = 0; i < MAX_THREADS; i++) {
            thread_entry_t *t = &thread_table[i];
            if (!t->active) continue;
            int is_child = 0;
            if (t->ptraced && t->tracer_tid == tracer_tid) is_child = 1;
            if (t->is_vm_clone && t->parent_tid == tracer_tid) is_child = 1;
            if (!is_child) continue;
            if (pid > 0 && t->guest_tid != pid) continue;

            pthread_cond_wait(&t->ptrace_cond, &thread_lock);
            break;  /* Re-scan after wakeup */
        }
    }
}
