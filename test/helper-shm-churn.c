/* helper-shm-churn.c — attach → partial-mprotect → detach in a loop.
 *
 * Each partial mprotect splits the segment's 2MB block into an L3 page-table
 * page; each detach must reclaim it. Run under test-diagnostics.sh, which
 * asserts hl never logs "page table pool exhausted" — the host-observable
 * signal that the L3 reclaim (H2) is working. 300 cycles exceeds the
 * 240-page pool, so without reclaim the pool is exhausted well before the end.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>

int main(void) {
    int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (id < 0) { printf("shmget failed\n"); return 1; }
    for (int i = 0; i < 300; i++) {
        char *p = shmat(id, (void *)0, 0);
        if (p == (char *)-1) { printf("shmat failed at %d\n", i); return 1; }
        *p = (char)i;
        mprotect(p, 4096, PROT_READ);   /* splits the 2MB block */
        shmdt(p);
    }
    shmctl(id, IPC_RMID, NULL);
    printf("churn-done\n");
    return 0;
}
