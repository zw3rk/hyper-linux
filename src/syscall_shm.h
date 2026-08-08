/*
 * syscall_shm.h — SysV shared memory (shmget/shmat/shmdt/shmctl) for MIT-SHM
 *
 * Copyright (c) 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Uses host SysV SHM so macOS XQuartz can shmat() the same shmid the guest
 * passes over the X protocol (required for XShmPutImage).
 */
#ifndef SYSCALL_SHM_H
#define SYSCALL_SHM_H

#include "guest.h"
#include <stdint.h>

int64_t sys_shmget(guest_t *g, int64_t key, uint64_t size, int shmflg);
int64_t sys_shmat(guest_t *g, int shmid, uint64_t shmaddr, int shmflg);
int64_t sys_shmdt(guest_t *g, uint64_t shmaddr);
int64_t sys_shmctl(guest_t *g, int shmid, int cmd, uint64_t buf_gva);

/* Fork support. Attached SysV segments must stay SHARED across fork (POSIX),
 * but the Stage-2 override that backs them lives in the parent's VM only and
 * segs[] is process-local, so a child would silently resolve SHM addresses to
 * its own COW copy of primary RAM. Export in the parent, re-establish in the
 * child. */
#define HL_SHM_FORK_MAX_ATTACH 8
typedef struct {
    int32_t  host_shmid;
    int32_t  nattach;
    int32_t  rmid_pending;
    int32_t  nattach_va;
    uint64_t size;
    uint64_t map_size;
    uint64_t ipa_span;
    uint64_t guest_va;
    uint64_t ipa;
    /* Every live attach window, not just the most recent — a segment mapped
     * twice must survive fork with both windows intact. */
    uint64_t attach_va[HL_SHM_FORK_MAX_ATTACH];
} hl_shm_fork_rec_t;

/* Returns the number of records written (<= max), or -1. */
int hl_shm_fork_export(hl_shm_fork_rec_t *out, int max);
/* Re-attach and re-map each record into this process's VM. */
int hl_shm_fork_import(guest_t *g, const hl_shm_fork_rec_t *in, int n);
int hl_shm_fork_max(void);

#endif /* SYSCALL_SHM_H */
