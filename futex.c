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

/* ---------- Syscall entry point ---------- */

int64_t sys_futex(guest_t *g, uint64_t uaddr, int op, uint32_t val,
                  uint64_t timeout_gva, uint64_t uaddr2, uint32_t val3) {
    (void)uaddr2;  /* REQUEUE not implemented */

    int cmd = op & FUTEX_CMD_MASK;

    switch (cmd) {
    case FUTEX_WAIT:
        return futex_wait(g, uaddr, val, timeout_gva,
                          FUTEX_BITSET_MATCH_ANY, /*is_absolute=*/0);

    case FUTEX_WAKE:
        return futex_wake(uaddr, val, FUTEX_BITSET_MATCH_ANY);

    case FUTEX_WAIT_BITSET:
        return futex_wait(g, uaddr, val, timeout_gva,
                          val3, /*is_absolute=*/1);

    case FUTEX_WAKE_BITSET:
        return futex_wake(uaddr, val, val3);

    default:
        /* Unimplemented futex operation — return ENOSYS so musl knows
         * to fall back (e.g., for FUTEX_CMP_REQUEUE). */
        return -LINUX_ENOSYS;
    }
}

int futex_wake_one(guest_t *g, uint64_t uaddr) {
    (void)g;
    return (int)futex_wake(uaddr, 1, FUTEX_BITSET_MATCH_ANY);
}
