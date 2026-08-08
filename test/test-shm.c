/* test-shm.c — SysV shared memory (shmget/shmat/shmdt/shmctl)
 *
 * The whole SysV SHM implementation shipped with no test at all. This
 * covers the basics plus the specific defects found in review:
 *   - attached segments must stay SHARED across fork (they resolved to the
 *     child's own COW copy of primary RAM)
 *   - failed attaches must not leak table slots (64 of them disabled SHM
 *     for the rest of the process)
 *   - detach/re-attach must reuse address space (the allocators were
 *     monotonic bump pointers that never reclaimed)
 *   - IPC_INFO/SHM_INFO must actually write their output buffer
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define SEG_SIZE 4096

int main(void) {
    int passes = 0, fails = 0;
    printf("test-shm: SysV shared memory\n");

    int id = -1;

    TEST("shmget(IPC_PRIVATE)");
    {
        id = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT | 0600);
        if (id >= 0) PASS();
        else FAILF("shmget: %s", strerror(errno));
    }
    if (id < 0) { SUMMARY("test-shm"); return 1; }

    TEST("shmat + write/read back");
    {
        char *p = shmat(id, NULL, 0);
        if (p == (char *)-1) FAILF("shmat: %s", strerror(errno));
        else {
            strcpy(p, "SHMDATA");
            if (strcmp(p, "SHMDATA") == 0) PASS();
            else FAIL("readback mismatch");
            shmdt(p);
        }
    }

    TEST("shmctl(IPC_STAT) reports the size");
    {
        struct shmid_ds ds;
        memset(&ds, 0, sizeof(ds));
        if (shmctl(id, IPC_STAT, &ds) != 0)
            FAILF("shmctl IPC_STAT: %s", strerror(errno));
        else if (ds.shm_segsz >= SEG_SIZE) PASS();
        else FAILF("segsz %llu < %d", (unsigned long long)ds.shm_segsz, SEG_SIZE);
    }

    /* The headline defect: a segment attached before fork must be the SAME
     * memory in the child, not a private copy. */
    TEST("segment is shared across fork");
    {
        char *p = shmat(id, NULL, 0);
        if (p == (char *)-1) FAILF("shmat: %s", strerror(errno));
        else {
            strcpy(p, "PARENT");
            pid_t pid = fork();
            if (pid < 0) FAILF("fork: %s", strerror(errno));
            else if (pid == 0) {
                /* Child sees the parent's write, and writes back. */
                int ok = (strcmp(p, "PARENT") == 0);
                strcpy(p, "CHILD");
                _exit(ok ? 0 : 2);
            } else {
                int st = 0;
                waitpid(pid, &st, 0);
                int child_saw = (WIFEXITED(st) && WEXITSTATUS(st) == 0);
                int parent_sees = (strcmp(p, "CHILD") == 0);
                if (child_saw && parent_sees) PASS();
                else FAILF("child_saw_parent=%d parent_sees_child=%d (%.8s)",
                           child_saw, parent_sees, p);
            }
            shmdt(p);
        }
    }

    TEST("detach then re-attach reuses address space");
    {
        int ok = 1;
        for (int i = 0; i < 40 && ok; i++) {
            char *p = shmat(id, NULL, 0);
            if (p == (char *)-1) {
                FAILF("shmat cycle %d: %s", i, strerror(errno));
                ok = 0;
                break;
            }
            *p = (char)i;
            if (shmdt(p) != 0) { FAILF("shmdt cycle %d", i); ok = 0; break; }
        }
        if (ok) PASS();
    }

    TEST("failed attaches do not exhaust the table");
    {
        /* Bogus ids must fail without consuming a slot. */
        for (int i = 0; i < 200; i++)
            (void)shmat(0x7fff0000 + i, NULL, 0);
        char *p = shmat(id, NULL, 0);
        if (p == (char *)-1) FAILF("shmat after failures: %s", strerror(errno));
        else { shmdt(p); PASS(); }
    }

    TEST("shmctl(IPC_INFO) fills the buffer");
    {
        struct shminfo si;
        memset(&si, 0xAA, sizeof(si));
        int rc = shmctl(0, IPC_INFO, (struct shmid_ds *)&si);
        if (rc < 0) FAILF("IPC_INFO: %s", strerror(errno));
        else if (si.shmmni != 0xAAAAAAAAAAAAAAAAULL && si.shmmni > 0) PASS();
        else FAILF("shminfo not written (shmmni=%llu)",
                   (unsigned long long)si.shmmni);
    }

    TEST("shmctl(IPC_RMID)");
    {
        if (shmctl(id, IPC_RMID, NULL) == 0) PASS();
        else FAILF("IPC_RMID: %s", strerror(errno));
    }

    SUMMARY("test-shm");
    return fails ? 1 : 0;
}
