/* futex.c — Linux futex emulation for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hash table of wait queues keyed by guest virtual address. Each bucket
 * has its own mutex for fine-grained locking. Waiters are singly-linked
 * lists with per-waiter condition variables for precise wakeup.
 *
 * Atomicity: The critical FUTEX_WAIT race (guest writes futex word after
 * our read but before we sleep) is prevented by holding the bucket lock
 * across the word-read + enqueue + cond_wait sequence. FUTEX_WAKE also
 * acquires the same bucket lock, so a wake can't slip between the read
 * and the wait.
 *
 * Timeout: FUTEX_WAIT with a non-NULL timeout uses pthread_cond_timedwait
 * with an absolute deadline. FUTEX_WAIT_BITSET always uses absolute time.
 */
#include "futex.h"
#include "syscall.h"      /* LINUX_E*, linux_timespec_t */
#include "syscall_proc.h" /* proc_get_pid (fallback TID for PI futex) */
#include "thread.h"       /* current_thread, guest_tid (for PI futex TID) */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* Interrupt flag: when set, futex_wait returns -EINTR. Used to
 * simulate SIGCHLD delivery when all CLONE_THREAD workers exit —
 * wakes the main thread from blocking futex_wait without triggering
 * a full exit_group. */
_Atomic int futex_interrupt_requested = 0;

/* ---------- Futex operations (from Linux uapi) ---------- */
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_LOCK_PI         6
#define FUTEX_UNLOCK_PI       7
#define FUTEX_TRYLOCK_PI      8
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CMD_MASK      0x7F

#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFU

/* PI futex word layout (bits):
 *   0-30: TID of lock holder (0 = unlocked)
 *   31:   FUTEX_WAITERS — at least one thread is blocked
 *
 * Linux kernel: FUTEX_WAITERS=0x80000000 (bit 31),
 * FUTEX_OWNER_DIED=0x40000000 (bit 30), FUTEX_TID_MASK=0x3FFFFFFF.
 * We omit OWNER_DIED (no robust futex support), so our TID mask is
 * 31 bits (0x7FFFFFFF). This matches Rosetta's UnfairLock convention. */
#define FUTEX_TID_MASK        0x7FFFFFFFU
#define FUTEX_WAITERS         0x80000000U

/* ---------- Hash table ---------- */

#define FUTEX_BUCKETS 64

/* Per-waiter node. Allocated on the host stack of the waiting thread
 * (no malloc needed — the waiter is stack-local to sys_futex). */
typedef struct futex_waiter {
    uint64_t              uaddr;   /* Guest VA being waited on */
    uint32_t              bitset;  /* For WAIT_BITSET matching */
    pthread_cond_t        cond;    /* Signalled by WAKE to unblock this waiter */
    int                   woken;   /* Set to 1 by WAKE before signalling */
    struct futex_waiter  *next;    /* Next waiter in same bucket */
} futex_waiter_t;

/* One bucket in the hash table. Protected by its own mutex.
 * Lock order: 7 (leaf locks, index-ordered when two acquired). */
typedef struct {
    pthread_mutex_t lock;
    futex_waiter_t *head;   /* Linked list of waiters hashing to this bucket */
} futex_bucket_t;

static futex_bucket_t buckets[FUTEX_BUCKETS];

/* Hash function: mix guest VA to a bucket index. Simple but effective —
 * futex addresses are typically 4-byte aligned, so shift off the low bits. */
static inline unsigned futex_hash(uint64_t uaddr) {
    return (unsigned)((uaddr >> 2) ^ (uaddr >> 14)) % FUTEX_BUCKETS;
}

/* ---------- Public API ---------- */

void futex_init(void) {
    for (int i = 0; i < FUTEX_BUCKETS; i++) {
        pthread_mutex_init(&buckets[i].lock, NULL);
        buckets[i].head = NULL;
    }
}

/* Convert a Linux guest timespec to an absolute struct timespec deadline.
 * For FUTEX_WAIT (relative timeout), adds the duration to the current time.
 * For FUTEX_WAIT_BITSET (absolute timeout), uses the value directly.
 * Returns 0 on success, -1 if the guest pointer is invalid. */
static int futex_make_deadline(guest_t *g, uint64_t timeout_gva,
                                int is_absolute, struct timespec *out) {
    linux_timespec_t lts;
    if (guest_read(g, timeout_gva, &lts, sizeof(lts)) < 0)
        return -1;

    if (is_absolute) {
        out->tv_sec  = (time_t)lts.tv_sec;
        out->tv_nsec = (long)lts.tv_nsec;
    } else {
        /* Relative: add to current CLOCK_REALTIME (pthread_cond_timedwait
         * uses CLOCK_REALTIME by default on macOS) */
        struct timeval now;
        gettimeofday(&now, NULL);
        out->tv_sec  = now.tv_sec + (time_t)lts.tv_sec;
        out->tv_nsec = (long)now.tv_usec * 1000 + (long)lts.tv_nsec;
        while (out->tv_nsec >= 1000000000L) {
            out->tv_sec  += 1;
            out->tv_nsec -= 1000000000L;
        }
    }
    return 0;
}

/* FUTEX_WAIT / FUTEX_WAIT_BITSET: atomically check word == val, then sleep. */
static int64_t futex_wait(guest_t *g, uint64_t uaddr, uint32_t expected,
                           uint64_t timeout_gva, uint32_t bitset,
                           int is_absolute) {
    if (bitset == 0) return -LINUX_EINVAL;

    unsigned idx = futex_hash(uaddr);
    futex_bucket_t *b = &buckets[idx];

    /* Build deadline before locking (avoid holding lock during syscall) */
    int has_timeout = (timeout_gva != 0);
    struct timespec deadline;
    if (has_timeout) {
        if (futex_make_deadline(g, timeout_gva, is_absolute, &deadline) < 0)
            return -LINUX_EFAULT;
    }

    pthread_mutex_lock(&b->lock);

    /* Atomically read the futex word while holding the bucket lock.
     * If it doesn't match, return EAGAIN immediately. */
    uint32_t *word = (uint32_t *)guest_ptr(g, uaddr);
    if (!word) {
        pthread_mutex_unlock(&b->lock);
        return -LINUX_EFAULT;
    }

    uint32_t current = __atomic_load_n(word, __ATOMIC_SEQ_CST);
    if (current != expected) {
        pthread_mutex_unlock(&b->lock);
        return -LINUX_EAGAIN;
    }

    /* Enqueue waiter (stack-allocated — lives on this thread's stack) */
    futex_waiter_t waiter = {
        .uaddr  = uaddr,
        .bitset = bitset,
        .woken  = 0,
        .next   = b->head,
    };
    pthread_cond_init(&waiter.cond, NULL);
    b->head = &waiter;

    /* Wait until woken or timeout */
    int ret = 0;

    /* Record start time for the no-timeout path. On real Linux, any
     * pending signal interrupts futex_wait with -EINTR. Without a
     * timer signal (SIGVTALRM from timer_create/setitimer), multi-
     * threaded programs like GHC can deadlock when a thread blocks in
     * futex_wait and no wakeup arrives (e.g., shutdown signal sent to
     * the wrong I/O manager). We return -EINTR after 1 second of
     * blocking to simulate periodic signal delivery. All futex callers
     * (musl, GHC RTS) handle -EINTR correctly by re-checking their
     * condition and retrying. */
    struct timeval wait_start;
    if (!has_timeout)
        gettimeofday(&wait_start, NULL);

    while (!waiter.woken) {
        if (has_timeout) {
            int rc = pthread_cond_timedwait(&waiter.cond, &b->lock, &deadline);
            if (rc != 0) {
                /* Timeout (ETIMEDOUT) or error — stop waiting */
                ret = -LINUX_ETIMEDOUT;
                break;
            }
        } else {
            /* No timeout specified — poll every 100ms to check for
             * exit_group, futex_interrupt (simulated SIGCHLD), or
             * excessive wait time (simulated signal interruption). */
            struct timeval now;
            gettimeofday(&now, NULL);
            struct timespec poll_ts = {
                .tv_sec  = now.tv_sec,
                .tv_nsec = (long)now.tv_usec * 1000 + 100000000L, /* +100ms */
            };
            if (poll_ts.tv_nsec >= 1000000000L) {
                poll_ts.tv_sec  += 1;
                poll_ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&waiter.cond, &b->lock, &poll_ts);

            if (atomic_load(&exit_group_requested) ||
                atomic_load(&futex_interrupt_requested)) {
                ret = -LINUX_EINTR;
                break;
            }

            /* Simulate periodic signal delivery: return -EINTR after
             * 1 second of blocking. This prevents deadlocks in multi-
             * threaded runtimes (GHC) that rely on signal-interrupted
             * futex_wait for scheduler context switching. */
            gettimeofday(&now, NULL);
            long elapsed_ms = (now.tv_sec - wait_start.tv_sec) * 1000
                            + (now.tv_usec - wait_start.tv_usec) / 1000;
            if (elapsed_ms >= 1000) {
                ret = -LINUX_EINTR;
                break;
            }
        }
    }

    /* Dequeue waiter. If woken=1, the wake/requeue operation already
     * unlinked us from the bucket list — skip dequeue. If woken=0
     * (timeout/interrupt), we're still in the list and must self-dequeue.
     *
     * For the self-dequeue path: requeue may have moved us to a different
     * bucket (changed waiter.uaddr), so re-hash. Race: between releasing
     * the old bucket lock and acquiring the new one, another requeue can
     * move the waiter again. Loop until we find and dequeue ourselves. */
    if (!waiter.woken) {
        for (;;) {
            unsigned dequeue_idx = futex_hash(waiter.uaddr);
            futex_bucket_t *b_dequeue = &buckets[dequeue_idx];
            if (b_dequeue != b) {
                pthread_mutex_unlock(&b->lock);
                pthread_mutex_lock(&b_dequeue->lock);
                b = b_dequeue;
            }
            /* Search for our waiter in the bucket */
            int found = 0;
            futex_waiter_t **pp = &b->head;
            while (*pp) {
                if (*pp == &waiter) {
                    *pp = waiter.next;
                    found = 1;
                    break;
                }
                pp = &(*pp)->next;
            }
            if (found) break;
            /* Not found — waiter was requeued again between our hash
             * computation and lock acquisition. Re-read uaddr and retry. */
        }
    }
    pthread_mutex_unlock(&b->lock);
    pthread_cond_destroy(&waiter.cond);

    if (waiter.woken) return 0;
    return ret;
}

/* FUTEX_WAKE / FUTEX_WAKE_BITSET: wake up to val waiters at uaddr.
 * Woken waiters are unlinked from the bucket list so subsequent
 * operations don't count them as still-sleeping entries. */
static int64_t futex_wake(uint64_t uaddr, uint32_t val, uint32_t bitset) {
    if (bitset == 0) return -LINUX_EINVAL;

    unsigned idx = futex_hash(uaddr);
    futex_bucket_t *b = &buckets[idx];
    int woken = 0;

    pthread_mutex_lock(&b->lock);

    futex_waiter_t **pp = &b->head;
    while (*pp && (uint32_t)woken < val) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == uaddr && (w->bitset & bitset) != 0) {
            *pp = w->next;  /* Unlink before signaling */
            w->woken = 1;
            pthread_cond_signal(&w->cond);
            woken++;
        } else {
            pp = &w->next;
        }
    }

    pthread_mutex_unlock(&b->lock);

    return woken;
}

/* FUTEX_REQUEUE / FUTEX_CMP_REQUEUE: wake val waiters at uaddr, then
 * move up to val2 remaining waiters from uaddr to uaddr2.
 *
 * CMP_REQUEUE additionally checks *uaddr == val3 before proceeding;
 * returns -EAGAIN if the comparison fails (stale wakeup avoidance).
 *
 * Musl uses FUTEX_REQUEUE (not CMP) in pthread_cond_timedwait.c for
 * efficient condition variable broadcast — avoids thundering herd by
 * moving waiters directly to the mutex futex instead of waking them all.
 *
 * Lock ordering: always acquire lower-indexed bucket first to avoid
 * deadlock when source and destination hash to different buckets. */
static int64_t futex_requeue(guest_t *g, uint64_t uaddr, uint32_t wake_count,
                              uint32_t requeue_count, uint64_t uaddr2,
                              int do_cmp, uint32_t expected) {
    unsigned idx_src = futex_hash(uaddr);
    unsigned idx_dst = futex_hash(uaddr2);
    futex_bucket_t *b_src = &buckets[idx_src];
    futex_bucket_t *b_dst = &buckets[idx_dst];

    /* Lock both buckets in consistent order (lower index first) */
    if (idx_src == idx_dst) {
        pthread_mutex_lock(&b_src->lock);
    } else if (idx_src < idx_dst) {
        pthread_mutex_lock(&b_src->lock);
        pthread_mutex_lock(&b_dst->lock);
    } else {
        pthread_mutex_lock(&b_dst->lock);
        pthread_mutex_lock(&b_src->lock);
    }

    /* CMP_REQUEUE: atomically verify *uaddr == expected */
    if (do_cmp) {
        uint32_t *word = (uint32_t *)guest_ptr(g, uaddr);
        if (!word) {
            if (idx_src != idx_dst) pthread_mutex_unlock(&b_dst->lock);
            pthread_mutex_unlock(&b_src->lock);
            return -LINUX_EFAULT;
        }
        uint32_t current = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        if (current != expected) {
            if (idx_src != idx_dst) pthread_mutex_unlock(&b_dst->lock);
            pthread_mutex_unlock(&b_src->lock);
            return -LINUX_EAGAIN;
        }
    }

    int woken = 0;
    int requeued = 0;

    /* Walk source bucket: wake up to wake_count, requeue up to requeue_count */
    futex_waiter_t **pp = &b_src->head;
    while (*pp) {
        futex_waiter_t *w = *pp;
        if (w->uaddr != uaddr) {
            pp = &w->next;
            continue;
        }

        if ((uint32_t)woken < wake_count) {
            /* Wake this waiter: unlink from source, then signal */
            *pp = w->next;
            w->woken = 1;
            pthread_cond_signal(&w->cond);
            woken++;
            /* Don't advance pp — *pp is already the next node */
        } else if ((uint32_t)requeued < requeue_count) {
            /* Requeue: remove from source, add to destination */
            *pp = w->next;
            w->uaddr = uaddr2;
            w->next = b_dst->head;
            b_dst->head = w;
            requeued++;
        } else {
            break;  /* Both limits reached */
        }
    }

    /* Unlock in reverse order */
    if (idx_src == idx_dst) {
        pthread_mutex_unlock(&b_src->lock);
    } else if (idx_src < idx_dst) {
        pthread_mutex_unlock(&b_dst->lock);
        pthread_mutex_unlock(&b_src->lock);
    } else {
        pthread_mutex_unlock(&b_src->lock);
        pthread_mutex_unlock(&b_dst->lock);
    }

    return woken + requeued;
}

/* FUTEX_WAKE_OP: atomically modify *uaddr2, wake val waiters at uaddr,
 * then conditionally wake val2 waiters at uaddr2 based on the old value.
 *
 * The op argument encodes: operation on *uaddr2 and comparison predicate.
 * Used by glibc's pthread_cond_signal; musl does NOT use this, but
 * we implement it for compatibility with glibc-linked binaries.
 *
 * val3 encodes both the operation and comparison:
 *   bits 28-31: op code (SET=0, ADD=1, OR=2, ANDN=3, XOR=4)
 *   bits 24-27: cmp code (EQ=0, NE=1, LT=2, LE=3, GT=4, GE=5)
 *   bits 12-23: op arg
 *   bits  0-11: cmp arg */
static int64_t futex_wake_op(guest_t *g, uint64_t uaddr, uint32_t val,
                              uint64_t uaddr2, uint32_t val2, uint32_t val3) {
    unsigned idx1 = futex_hash(uaddr);
    unsigned idx2 = futex_hash(uaddr2);
    futex_bucket_t *b1 = &buckets[idx1];
    futex_bucket_t *b2 = &buckets[idx2];

    /* Lock ordering */
    if (idx1 == idx2) {
        pthread_mutex_lock(&b1->lock);
    } else if (idx1 < idx2) {
        pthread_mutex_lock(&b1->lock);
        pthread_mutex_lock(&b2->lock);
    } else {
        pthread_mutex_lock(&b2->lock);
        pthread_mutex_lock(&b1->lock);
    }

    /* Decode operation and comparison from val3.
     * Bits 31-28: operation (bit 31 = OPARG_SHIFT flag, bits 30-28 = op)
     * Bits 27-24: comparison operator
     * Bits 23-12: op_arg (operand for modify, 12-bit signed)
     * Bits 11-0:  cmp_arg (operand for compare, 12-bit signed)
     * Both op_arg and cmp_arg are sign-extended from 12 bits to match
     * the Linux kernel's sign_extend32() in futex_atomic_op_inuser(). */
    unsigned wake_op   = (val3 >> 28) & 0xF;
    unsigned wake_cmp  = (val3 >> 24) & 0xF;
    int op_arg_raw     = (int)((val3 >> 12) & 0xFFF);
    int op_arg         = (op_arg_raw << 20) >> 20;  /* Sign-extend 12→32 */
    int cmp_arg_raw    = (int)(val3 & 0xFFF);
    int cmp_arg        = (cmp_arg_raw << 20) >> 20; /* Sign-extend 12→32 */

    /* FUTEX_OP_OPARG_SHIFT (bit 3 of wake_op): interpret op_arg as 1<<op_arg */
    int op_shift = (int)(wake_op & 8);
    wake_op &= 7;  /* Actual operation is bits 0-2 */
    if (op_shift) op_arg = (int)(1U << (op_arg & 0x1F));

    /* Atomically modify *uaddr2 */
    uint32_t *word2 = (uint32_t *)guest_ptr(g, uaddr2);
    if (!word2) {
        if (idx1 != idx2) pthread_mutex_unlock(&b2->lock);
        pthread_mutex_unlock(&b1->lock);
        return -LINUX_EFAULT;
    }

    /* Atomic read-modify-write on *uaddr2 using CAS loop.
     * Matches Linux kernel's futex_atomic_op_inuser() semantics:
     * the modification must be atomic w.r.t. concurrent guest stores. */
    uint32_t old_val;
    uint32_t new_val;
    do {
        old_val = __atomic_load_n(word2, __ATOMIC_SEQ_CST);
        switch (wake_op) {
        case 0: new_val = op_arg;             break;  /* SET */
        case 1: new_val = old_val + op_arg;   break;  /* ADD */
        case 2: new_val = old_val | op_arg;   break;  /* OR */
        case 3: new_val = old_val & ~op_arg;  break;  /* ANDN */
        case 4: new_val = old_val ^ op_arg;   break;  /* XOR */
        default: new_val = old_val;           break;
        }
    } while (!__atomic_compare_exchange_n(word2, &old_val, new_val,
                                           /*weak=*/0,
                                           __ATOMIC_SEQ_CST,
                                           __ATOMIC_SEQ_CST));

    /* Wake up to val waiters at uaddr (unlink woken entries) */
    int woken = 0;
    futex_waiter_t **pp1 = &b1->head;
    while (*pp1 && (uint32_t)woken < val) {
        futex_waiter_t *w = *pp1;
        if (w->uaddr == uaddr) {
            *pp1 = w->next;
            w->woken = 1;
            pthread_cond_signal(&w->cond);
            woken++;
        } else {
            pp1 = &w->next;
        }
    }

    /* Evaluate comparison predicate on old_val */
    int cond_met = 0;
    /* Linux FUTEX_WAKE_OP uses signed comparison semantics */
    int32_t sv = (int32_t)old_val, sa = (int32_t)cmp_arg;
    switch (wake_cmp) {
    case 0: cond_met = (sv == sa); break;  /* EQ */
    case 1: cond_met = (sv != sa); break;  /* NE */
    case 2: cond_met = (sv <  sa); break;  /* LT (signed) */
    case 3: cond_met = (sv <= sa); break;  /* LE (signed) */
    case 4: cond_met = (sv >  sa); break;  /* GT (signed) */
    case 5: cond_met = (sv >= sa); break;  /* GE (signed) */
    default: break;
    }

    /* Conditionally wake up to val2 waiters at uaddr2 (unlink woken) */
    if (cond_met) {
        futex_waiter_t **pp2 = &b2->head;
        int woken2 = 0;
        while (*pp2 && (uint32_t)woken2 < val2) {
            futex_waiter_t *w2 = *pp2;
            if (w2->uaddr == uaddr2) {
                *pp2 = w2->next;
                w2->woken = 1;
                pthread_cond_signal(&w2->cond);
                woken2++;
            } else {
                pp2 = &w2->next;
            }
        }
        woken += woken2;
    }

    /* Unlock reverse order */
    if (idx1 == idx2) {
        pthread_mutex_unlock(&b1->lock);
    } else if (idx1 < idx2) {
        pthread_mutex_unlock(&b2->lock);
        pthread_mutex_unlock(&b1->lock);
    } else {
        pthread_mutex_unlock(&b1->lock);
        pthread_mutex_unlock(&b2->lock);
    }

    return woken;
}

/* ---------- PI (Priority-Inheritance) futex ----------
 *
 * PI futexes use the futex word itself as an atomic lock:
 *   bits 0-30 = owner TID (FUTEX_TID_MASK), bit 31 = FUTEX_WAITERS
 *
 * We don't implement real priority inheritance (boosting the holder's
 * priority to the highest waiter's), but we implement the locking
 * semantics correctly. Rosetta's JIT runtime uses PI futexes for its
 * internal locks — it only needs the mutex behavior, not the RT priority
 * boosting. Waiters block on a per-address condition variable (reusing
 * the same bucket hash table as normal futexes). */

/* FUTEX_LOCK_PI: Block until the lock at uaddr can be acquired.
 *
 * The PI futex word stores the owner TID in bits 0-30 and a WAITERS
 * flag in bit 31. The kernel (us) sets FUTEX_WAITERS when a thread
 * blocks, so the current owner knows to call FUTEX_UNLOCK_PI (slow
 * path) instead of just doing a userspace CAS(TID→0) (fast path).
 *
 * Flow: try CAS(0→TID). If held by another thread, set WAITERS bit
 * via CAS, then block. On wakeup, retry acquisition. */
static int64_t futex_lock_pi(guest_t *g, uint64_t uaddr,
                              uint64_t timeout_gva) {
    uint32_t *word = (uint32_t *)guest_ptr(g, uaddr);
    if (!word) return -LINUX_EFAULT;

    uint32_t tid = current_thread ? (uint32_t)current_thread->guest_tid
                                  : (uint32_t)proc_get_pid();

    /* Build deadline (if timeout specified, it's absolute CLOCK_REALTIME) */
    int has_timeout = (timeout_gva != 0);
    struct timespec deadline;
    if (has_timeout) {
        if (futex_make_deadline(g, timeout_gva, /*is_absolute=*/1, &deadline) < 0)
            return -LINUX_EFAULT;
    }

    unsigned idx = futex_hash(uaddr);
    futex_bucket_t *b = &buckets[idx];

    for (;;) {
        /* Fast path: try to CAS 0 → our TID (uncontended acquisition) */
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(word, &expected, tid,
                                         /*weak=*/0,
                                         __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST)) {
            return 0;  /* Acquired */
        }

        /* Already own it? Deadlock (Linux returns EDEADLK) */
        if ((expected & FUTEX_TID_MASK) == tid)
            return -LINUX_EDEADLK;

        /* Check if the owner thread has exited without releasing the lock.
         * Linux kernel handles this via PI futex cleanup on thread exit;
         * we detect it lazily here since we don't track PI ownership
         * per-thread. Clear the futex word and retry acquisition. */
        uint32_t owner_tid = expected & FUTEX_TID_MASK;
        if (owner_tid != 0 && !thread_find((int64_t)owner_tid)) {
            __atomic_compare_exchange_n(word, &expected, 0,
                                         /*weak=*/0,
                                         __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST);
            continue;  /* Retry acquisition */
        }

        /* Set the WAITERS bit so the owner takes the slow unlock path
         * (calls FUTEX_UNLOCK_PI instead of just CAS to 0). Retry
         * the CAS in a loop since the owner may release concurrently. */
        for (;;) {
            uint32_t cur = __atomic_load_n(word, __ATOMIC_SEQ_CST);
            if ((cur & FUTEX_TID_MASK) == 0)
                break;  /* Owner released — retry outer loop */
            if (cur & FUTEX_WAITERS)
                break;  /* Already set by another waiter */
            uint32_t desired = cur | FUTEX_WAITERS;
            if (__atomic_compare_exchange_n(word, &cur, desired,
                                             /*weak=*/0,
                                             __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST))
                break;  /* WAITERS bit set */
        }

        /* Re-check after WAITERS bit: if lock is now free, retry */
        uint32_t cur = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        if ((cur & FUTEX_TID_MASK) == 0)
            continue;

        /* Enqueue and block */
        pthread_mutex_lock(&b->lock);

        /* Double-check under bucket lock: owner may have released
         * and called UNLOCK_PI between our WAITERS set and lock. */
        cur = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        if ((cur & FUTEX_TID_MASK) == 0) {
            pthread_mutex_unlock(&b->lock);
            continue;
        }

        futex_waiter_t waiter = {
            .uaddr  = uaddr,
            .bitset = FUTEX_BITSET_MATCH_ANY,
            .woken  = 0,
            .next   = b->head,
        };
        pthread_cond_init(&waiter.cond, NULL);
        b->head = &waiter;

        int owner_died = 0;
        while (!waiter.woken) {
            if (has_timeout) {
                int rc = pthread_cond_timedwait(&waiter.cond, &b->lock,
                                                 &deadline);
                if (rc != 0 && !waiter.woken) {
                    /* Timeout — dequeue and return */
                    futex_waiter_t **pp = &b->head;
                    while (*pp) {
                        if (*pp == &waiter) { *pp = waiter.next; break; }
                        pp = &(*pp)->next;
                    }
                    /* Only clear WAITERS bit if no waiters for this address */
                    int has_waiters = 0;
                    for (futex_waiter_t *w = b->head; w; w = w->next) {
                        if (w->uaddr == uaddr) { has_waiters = 1; break; }
                    }
                    pthread_mutex_unlock(&b->lock);
                    pthread_cond_destroy(&waiter.cond);
                    if (!has_waiters) {
                        for (;;) {
                            uint32_t v = __atomic_load_n(word, __ATOMIC_SEQ_CST);
                            if (!(v & FUTEX_WAITERS)) break;
                            uint32_t nv = v & ~FUTEX_WAITERS;
                            if (__atomic_compare_exchange_n(word, &v, nv,
                                                             /*weak=*/0,
                                                             __ATOMIC_SEQ_CST,
                                                             __ATOMIC_SEQ_CST))
                                break;
                        }
                    }
                    return -LINUX_ETIMEDOUT;
                }
            } else {
                /* No timeout: poll every 100ms to check exit_group
                 * and dead lock owners. */
                struct timeval now;
                gettimeofday(&now, NULL);
                struct timespec poll_ts = {
                    .tv_sec  = now.tv_sec,
                    .tv_nsec = (long)now.tv_usec * 1000 + 100000000L,
                };
                if (poll_ts.tv_nsec >= 1000000000L) {
                    poll_ts.tv_sec  += 1;
                    poll_ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&waiter.cond, &b->lock, &poll_ts);

                if (atomic_load(&exit_group_requested)) {
                    /* Dequeue and return */
                    futex_waiter_t **pp2 = &b->head;
                    while (*pp2) {
                        if (*pp2 == &waiter) { *pp2 = waiter.next; break; }
                        pp2 = &(*pp2)->next;
                    }
                    pthread_mutex_unlock(&b->lock);
                    pthread_cond_destroy(&waiter.cond);
                    return -LINUX_EINTR;
                }

                /* Check if the owner thread has died while we were
                 * waiting. If so, clear the lock and retry.
                 * Use thread_tid_alive (lock-free) instead of thread_find
                 * to avoid lock order inversion: bucket lock(7) is held
                 * here, and thread_find acquires thread_lock(5). */
                uint32_t check = __atomic_load_n(word, __ATOMIC_SEQ_CST);
                uint32_t check_tid = check & FUTEX_TID_MASK;
                if (check_tid != 0 && !thread_tid_alive((int64_t)check_tid)) {
                    owner_died = 1;
                    break;
                }
            }
        }

        /* Dequeue waiter from bucket list */
        futex_waiter_t **pp = &b->head;
        while (*pp) {
            if (*pp == &waiter) { *pp = waiter.next; break; }
            pp = &(*pp)->next;
        }
        pthread_mutex_unlock(&b->lock);
        pthread_cond_destroy(&waiter.cond);

        if (owner_died) {
            /* Clear the dead owner's lock word and retry acquisition */
            uint32_t v = __atomic_load_n(word, __ATOMIC_SEQ_CST);
            __atomic_compare_exchange_n(word, &v, 0,
                                         /*weak=*/0,
                                         __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST);
            continue;
        }

        /* Woken: retry acquisition. The WAITERS bit may remain set if
         * other waiters exist — the CAS(0→TID) will still succeed
         * since bit 31 is separate from the TID field. */
    }
}

/* FUTEX_TRYLOCK_PI: Non-blocking version of LOCK_PI.
 * CAS 0 → TID; if the lock is held, return -EAGAIN immediately. */
static int64_t futex_trylock_pi(guest_t *g, uint64_t uaddr) {
    uint32_t *word = (uint32_t *)guest_ptr(g, uaddr);
    if (!word) return -LINUX_EFAULT;

    uint32_t tid = current_thread ? (uint32_t)current_thread->guest_tid
                                  : (uint32_t)proc_get_pid();

    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(word, &expected, tid,
                                     /*weak=*/0,
                                     __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST)) {
        return 0;  /* Acquired */
    }

    return -LINUX_EAGAIN;  /* Lock held, can't acquire */
}

/* FUTEX_UNLOCK_PI: Release the PI lock at uaddr and wake one waiter.
 *
 * Called by the lock owner when FUTEX_WAITERS is set (slow unlock path).
 * Atomically clear the word to 0 (releasing the lock + clearing WAITERS),
 * then wake one blocked waiter so it can retry CAS(0→TID) acquisition. */
static int64_t futex_unlock_pi(guest_t *g, uint64_t uaddr) {
    uint32_t *word = (uint32_t *)guest_ptr(g, uaddr);
    if (!word) return -LINUX_EFAULT;

    uint32_t tid = current_thread ? (uint32_t)current_thread->guest_tid
                                  : (uint32_t)proc_get_pid();

    /* Verify we own the lock (TID field matches) */
    uint32_t cur = __atomic_load_n(word, __ATOMIC_SEQ_CST);
    if ((cur & FUTEX_TID_MASK) != tid)
        return -LINUX_EPERM;

    /* Atomically release: set word to 0 (clear TID + WAITERS flag).
     * Use CAS loop in case another thread is concurrently setting WAITERS. */
    for (;;) {
        uint32_t v = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        if (__atomic_compare_exchange_n(word, &v, 0,
                                         /*weak=*/0,
                                         __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST))
            break;
    }

    /* Wake one waiter so it can retry acquisition */
    unsigned idx = futex_hash(uaddr);
    futex_bucket_t *b = &buckets[idx];

    pthread_mutex_lock(&b->lock);
    futex_waiter_t **pp = &b->head;
    while (*pp) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == uaddr) {
            *pp = w->next;  /* Unlink before signaling */
            w->woken = 1;
            pthread_cond_signal(&w->cond);
            break;  /* Wake exactly one */
        }
        pp = &w->next;
    }
    pthread_mutex_unlock(&b->lock);

    return 0;
}

/* ---------- Syscall entry point ---------- */

int64_t sys_futex(guest_t *g, uint64_t uaddr, int op, uint32_t val,
                  uint64_t timeout_gva, uint64_t uaddr2, uint32_t val3) {
    int cmd = op & FUTEX_CMD_MASK;

    switch (cmd) {
    case FUTEX_WAIT:
        return futex_wait(g, uaddr, val, timeout_gva,
                          FUTEX_BITSET_MATCH_ANY, /*is_absolute=*/0);

    case FUTEX_WAKE:
        return futex_wake(uaddr, val, FUTEX_BITSET_MATCH_ANY);

    case FUTEX_REQUEUE:
        /* For REQUEUE, the timeout arg is repurposed as val2 (requeue count) */
        return futex_requeue(g, uaddr, val, (uint32_t)timeout_gva,
                              uaddr2, /*do_cmp=*/0, 0);

    case FUTEX_CMP_REQUEUE:
        /* Same repurposing of timeout → val2, plus compare against val3 */
        return futex_requeue(g, uaddr, val, (uint32_t)timeout_gva,
                              uaddr2, /*do_cmp=*/1, val3);

    case FUTEX_WAKE_OP:
        /* timeout arg repurposed as val2 (wake count for uaddr2) */
        return futex_wake_op(g, uaddr, val, uaddr2,
                              (uint32_t)timeout_gva, val3);

    case FUTEX_WAIT_BITSET:
        return futex_wait(g, uaddr, val, timeout_gva,
                          val3, /*is_absolute=*/1);

    case FUTEX_WAKE_BITSET:
        return futex_wake(uaddr, val, val3);

    case FUTEX_LOCK_PI:
        return futex_lock_pi(g, uaddr, timeout_gva);

    case FUTEX_UNLOCK_PI:
        return futex_unlock_pi(g, uaddr);

    case FUTEX_TRYLOCK_PI:
        return futex_trylock_pi(g, uaddr);

    default:
        /* Unimplemented futex operation (robust futexes, PI requeue).
         * Return ENOSYS so musl knows to fall back. */
        return -LINUX_ENOSYS;
    }
}

int futex_wake_one(guest_t *g, uint64_t uaddr) {
    (void)g;
    return (int)futex_wake(uaddr, 1, FUTEX_BITSET_MATCH_ANY);
}
