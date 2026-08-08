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

    /* The VA band is 2GB of 2MB windows — 1024 of them — so 40 cycles could
     * not exhaust it even with a completely leaking allocator, and the old
     * "no error in 40 tries" check proved nothing. Exceed the band, and
     * assert the address is REUSED rather than merely handed out: first-fit
     * over a correctly released band must return the same window each time.
     * A monotonic allocator fails on the address check immediately and on
     * exhaustion by cycle 1024. */
    TEST("detach then re-attach reuses the same address, 1100 times");
    {
        int ok = 1;
        char *first = NULL;
        for (int i = 0; i < 1100 && ok; i++) {
            char *p = shmat(id, NULL, 0);
            if (p == (char *)-1) {
                FAILF("shmat cycle %d: %s (address space exhausted?)",
                      i, strerror(errno));
                ok = 0;
                break;
            }
            if (!first) first = p;
            else if (p != first) {
                FAILF("cycle %d got %p, first was %p — window not reclaimed",
                      i, (void *)p, (void *)first);
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

    /* (-) Two windows onto one segment: the second shmat() overwrote the
     * single stored guest_va, orphaning the first. The VA allocator then
     * handed that still-mapped window to a DIFFERENT segment, and because
     * guest_map_va_range leaves an already-valid L2 block alone, the new
     * mapping was a silent no-op — the new segment's writes landed in the
     * old segment's shared pages. Observed: w == v1 and seg1 clobbered. */
    TEST("a second segment never reuses a live attach window");
    {
        char *v1 = shmat(id, NULL, 0);
        char *v2 = shmat(id, NULL, 0);
        if (v1 == (char *)-1 || v2 == (char *)-1) {
            FAILF("double shmat: %s", strerror(errno));
        } else {
            memcpy(v1, "SEG1DATA", 9);
            int id2 = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT | 0600);
            char *w = (id2 >= 0) ? shmat(id2, NULL, 0) : (char *)-1;
            if (w == (char *)-1) {
                FAILF("second segment: %s", strerror(errno));
            } else {
                memcpy(w, "SEG2DATA", 9);
                if (w == v1 || w == v2)
                    FAILF("new segment reused a live window (w=%p v1=%p v2=%p)",
                          (void *)w, (void *)v1, (void *)v2);
                else if (memcmp(v1, "SEG1DATA", 9) != 0)
                    FAILF("segment 1 clobbered by segment 2 (%.8s)", v1);
                else PASS();
                shmdt(w);
            }
            if (id2 >= 0) shmctl(id2, IPC_RMID, NULL);

            /* (+) Detaching one window must leave the other usable — the
             * teardown used to be all-or-nothing on the stored guest_va. */
            TEST("detaching one window leaves the other mapped");
            if (shmdt(v1) != 0) FAILF("shmdt(v1): %s", strerror(errno));
            else {
                memcpy(v2, "STILLOK", 8);
                if (memcmp(v2, "STILLOK", 8) == 0) PASS();
                else FAIL("surviving window stopped resolving");
            }
            shmdt(v2);
        }
    }

    /* (-) An explicit shmaddr onto an occupied window returned that address
     * with rc=0 while the guest kept reading the PREVIOUS segment: the L2
     * block was already valid, so installing the new one was skipped. */
    TEST("shmat at an occupied address fails instead of aliasing");
    {
        char *v = shmat(id, NULL, 0);
        int id2 = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT | 0600);
        if (v == (char *)-1 || id2 < 0) {
            FAILF("setup: %s", strerror(errno));
        } else {
            memcpy(v, "ORIGINAL", 9);
            errno = 0;
            char *bad = shmat(id2, v, 0);
            if (bad == (char *)-1 && errno == EINVAL) PASS();
            else if (bad == (char *)-1)
                FAILF("failed with %s, want EINVAL", strerror(errno));
            else
                FAILF("aliased at %p; segment 1 now reads %.8s",
                      (void *)bad, v);
            shmdt(v);
        }
        if (id2 >= 0) shmctl(id2, IPC_RMID, NULL);
    }

    /* (-) shmget in the parent, shmat in the child. hl forks via
     * posix_spawn, so the child's getpid() never equals the creator pid the
     * kernel recorded; the child fell into the "adopt a foreign shmid" path
     * and was rejected with EACCES. Only segments already ATTACHED at fork
     * time were carried across. */
    TEST("a segment created before fork is attachable in the child");
    {
        int id2 = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT | 0600);
        if (id2 < 0) FAILF("shmget: %s", strerror(errno));
        else {
            pid_t pid = fork();
            if (pid < 0) FAILF("fork: %s", strerror(errno));
            else if (pid == 0) {
                char *c = shmat(id2, NULL, 0);
                if (c == (char *)-1) _exit(3);
                memcpy(c, "FROMCHILD", 10);
                shmdt(c);
                _exit(0);
            } else {
                int st = 0;
                waitpid(pid, &st, 0);
                char *pv = shmat(id2, NULL, 0);
                if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
                    FAILF("child shmat failed (status 0x%x)", st);
                else if (pv == (char *)-1)
                    FAILF("parent shmat: %s", strerror(errno));
                else if (memcmp(pv, "FROMCHILD", 10) == 0) PASS();
                else FAILF("child write not visible (%.9s)", pv);
                if (pv != (char *)-1) shmdt(pv);
            }
            shmctl(id2, IPC_RMID, NULL);
        }
    }

    TEST("shmctl(IPC_RMID)");
    {
        if (shmctl(id, IPC_RMID, NULL) == 0) PASS();
        else FAILF("IPC_RMID: %s", strerror(errno));
    }

    SUMMARY("test-shm");
    return fails ? 1 : 0;
}
