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

#endif /* SYSCALL_SHM_H */
