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
#include "syscall.h"  /* LINUX_E*, linux_timespec_t */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* ---------- Futex operations (from Linux uapi) ---------- */
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CMD_MASK      0x7F

#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFU

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

/* One bucket in the hash table. Protected by its own mutex. */
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
        out->tv_nsec = now.tv_usec * 1000 + (long)lts.tv_nsec;
        if (out->tv_nsec >= 1000000000L) {
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
    while (!waiter.woken) {
        if (has_timeout) {
            int rc = pthread_cond_timedwait(&waiter.cond, &b->lock, &deadline);
            if (rc != 0) {
                /* Timeout (ETIMEDOUT) or error — stop waiting */
                ret = -LINUX_ETIMEDOUT;
                break;
            }
        } else {
            pthread_cond_wait(&waiter.cond, &b->lock);
        }
    }

    /* Dequeue waiter from the bucket list */
    futex_waiter_t **pp = &b->head;
    while (*pp) {
        if (*pp == &waiter) {
            *pp = waiter.next;
            break;
        }
        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&b->lock);
    pthread_cond_destroy(&waiter.cond);

    /* If we were woken by FUTEX_WAKE, return 0 regardless of timeout race */
    if (waiter.woken) return 0;
    return ret;
}

/* FUTEX_WAKE / FUTEX_WAKE_BITSET: wake up to val waiters at uaddr. */
static int64_t futex_wake(uint64_t uaddr, uint32_t val, uint32_t bitset) {
    if (bitset == 0) return -LINUX_EINVAL;

    unsigned idx = futex_hash(uaddr);
    futex_bucket_t *b = &buckets[idx];
    int woken = 0;

    pthread_mutex_lock(&b->lock);

    futex_waiter_t *w = b->head;
    while (w && (uint32_t)woken < val) {
        if (w->uaddr == uaddr && (w->bitset & bitset) != 0) {
            w->woken = 1;
            pthread_cond_signal(&w->cond);
            woken++;
        }
        w = w->next;
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
            /* Wake this waiter */
            w->woken = 1;
            pthread_cond_signal(&w->cond);
            woken++;
            pp = &w->next;  /* waiter removes itself from list on wakeup */
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

    /* Decode operation and comparison from val3 */
    unsigned wake_op   = (val3 >> 28) & 0xF;
    unsigned wake_cmp  = (val3 >> 24) & 0xF;
    unsigned op_arg    = (val3 >> 12) & 0xFFF;
    unsigned cmp_arg   = val3 & 0xFFF;

    /* Atomically modify *uaddr2 */
    uint32_t *word2 = (uint32_t *)guest_ptr(g, uaddr2);
    if (!word2) {
        if (idx1 != idx2) pthread_mutex_unlock(&b2->lock);
        pthread_mutex_unlock(&b1->lock);
        return -LINUX_EFAULT;
    }

    uint32_t old_val;
    uint32_t new_val;
    old_val = __atomic_load_n(word2, __ATOMIC_SEQ_CST);

    switch (wake_op) {
    case 0: new_val = op_arg;             break;  /* SET */
    case 1: new_val = old_val + op_arg;   break;  /* ADD */
    case 2: new_val = old_val | op_arg;   break;  /* OR */
    case 3: new_val = old_val & ~op_arg;  break;  /* ANDN */
    case 4: new_val = old_val ^ op_arg;   break;  /* XOR */
    default: new_val = old_val;           break;
    }
    __atomic_store_n(word2, new_val, __ATOMIC_SEQ_CST);

    /* Wake up to val waiters at uaddr */
    int woken = 0;
    futex_waiter_t *w = b1->head;
    while (w && (uint32_t)woken < val) {
        if (w->uaddr == uaddr) {
            w->woken = 1;
            pthread_cond_signal(&w->cond);
            woken++;
        }
        w = w->next;
    }

    /* Evaluate comparison predicate on old_val */
    int cond_met = 0;
    switch (wake_cmp) {
    case 0: cond_met = (old_val == cmp_arg); break;  /* EQ */
    case 1: cond_met = (old_val != cmp_arg); break;  /* NE */
    case 2: cond_met = (old_val <  cmp_arg); break;  /* LT */
    case 3: cond_met = (old_val <= cmp_arg); break;  /* LE */
    case 4: cond_met = (old_val >  cmp_arg); break;  /* GT */
    case 5: cond_met = (old_val >= cmp_arg); break;  /* GE */
    default: break;
    }

    /* Conditionally wake up to val2 waiters at uaddr2 */
    if (cond_met) {
        w = b2->head;
        int woken2 = 0;
        while (w && (uint32_t)woken2 < val2) {
            if (w->uaddr == uaddr2) {
                w->woken = 1;
                pthread_cond_signal(&w->cond);
                woken2++;
            }
            w = w->next;
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

    default:
        /* Unimplemented futex operation (PI futexes, robust futexes).
         * Return ENOSYS so musl knows to fall back. */
        return -LINUX_ENOSYS;
    }
}

int futex_wake_one(guest_t *g, uint64_t uaddr) {
    (void)g;
    return (int)futex_wake(uaddr, 1, FUTEX_BITSET_MATCH_ANY);
}
