/* futex.h — Linux futex emulation for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hash table of wait queues keyed by guest virtual address. Supports
 * FUTEX_WAIT, FUTEX_WAKE, FUTEX_WAIT_BITSET, and FUTEX_WAKE_BITSET.
 * Each waiter has its own condition variable for precise wakeup.
 */
#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>
#include "guest.h"

/* Initialize the futex subsystem. Call once at startup. */
void futex_init(void);

/* Main futex syscall entry point.
 * op:    futex operation (FUTEX_WAIT, FUTEX_WAKE, etc.)
 * uaddr: guest virtual address of the futex word
 * val:   expected value (WAIT) or max wakeups (WAKE)
 * timeout_gva: guest pointer to timespec (or 0 for no timeout)
 * uaddr2: second futex address (for REQUEUE, unused in MVP)
 * val3:  bitset (for WAIT_BITSET/WAKE_BITSET)
 * Returns 0 on success, negative Linux errno on failure. */
int64_t sys_futex(guest_t *g, uint64_t uaddr, int op, uint32_t val,
                  uint64_t timeout_gva, uint64_t uaddr2, uint32_t val3);

/* Wake up to 1 waiter at uaddr. Used by thread exit for
 * CLONE_CHILD_CLEARTID cleanup. Returns number of waiters woken. */
int futex_wake_one(guest_t *g, uint64_t uaddr);

#endif /* FUTEX_H */
