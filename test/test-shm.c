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
#include <sys/mman.h>
#include <signal.h>
#include <stdint.h>

#define SEG_SIZE 4096
#define HL_SHM_2MB (2u * 1024 * 1024)

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

    /* (H1) An explicit shmaddr onto the guest's OWN mapping must fail with
     * EINVAL and leave that mapping intact. The round-3 regression unmapped
     * the guest's blocks (SIGSEGV) because the failure cleanup tore down the
     * whole span, including blocks it never installed. */
    TEST("explicit shmaddr onto own mmap fails, mapping survives");
    {
        void *m = mmap(NULL, 2 * HL_SHM_2MB, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { FAILF("mmap: %s", strerror(errno)); }
        else {
            uintptr_t a = ((uintptr_t)m + (HL_SHM_2MB - 1)) & ~(uintptr_t)(HL_SHM_2MB - 1);
            memset((void *)a, 0xA5, HL_SHM_2MB);
            int id2 = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT | 0600);
            errno = 0;
            void *r = (id2 >= 0) ? shmat(id2, (void *)a, 0) : (void *)-1;
            if (id2 < 0) FAILF("shmget: %s", strerror(errno));
            else if (r != (void *)-1) FAILF("aliased at %p instead of EINVAL", r);
            else if (errno != EINVAL) FAILF("errno %s, want EINVAL", strerror(errno));
            else {
                volatile unsigned char *p = (volatile unsigned char *)a;
                if (p[0] == 0xA5 && p[HL_SHM_2MB - 1] == 0xA5) PASS();
                else FAIL("own mapping was clobbered/unmapped by shmat");
            }
            if (id2 >= 0) shmctl(id2, IPC_RMID, NULL);
            munmap(m, 2 * HL_SHM_2MB);
        }
    }

    /* (H1) Collision in the SECOND block of a 2-block attach: block[0] is a
     * hole (munmap'd), block[1] overlaps a live mapping. Must be rejected
     * wholesale (the preflight is a range overlap, per-block). */
    TEST("explicit shmaddr with a 2nd-block collision is rejected");
    {
        void *m = mmap(NULL, 2 * HL_SHM_2MB, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { FAILF("mmap: %s", strerror(errno)); }
        else {
            uintptr_t base = ((uintptr_t)m + (HL_SHM_2MB - 1)) & ~(uintptr_t)(HL_SHM_2MB - 1);
            memset((void *)(base + HL_SHM_2MB), 0x5A, HL_SHM_2MB);  /* mark 2nd block */
            munmap((void *)base, HL_SHM_2MB);          /* free block[0] only */
            int id2 = shmget(IPC_PRIVATE, 3 * 1024 * 1024, IPC_CREAT | 0600); /* 2 blocks */
            errno = 0;
            void *r = (id2 >= 0) ? shmat(id2, (void *)base, 0) : (void *)-1;
            if (id2 < 0) FAILF("shmget: %s", strerror(errno));
            else if (r != (void *)-1) FAILF("aliased at %p (2nd block collided)", r);
            else if (errno != EINVAL) FAILF("errno %s, want EINVAL", strerror(errno));
            else if (*(volatile unsigned char *)(base + HL_SHM_2MB) != 0x5A)
                FAIL("the live 2nd-block mapping was clobbered");
            else PASS();
            if (id2 >= 0) shmctl(id2, IPC_RMID, NULL);
            munmap((void *)(base + HL_SHM_2MB), HL_SHM_2MB);
        }
    }

    /* (+) An explicit shmaddr at a genuinely free address still works: the
     * preflight must reject only a LIVE overlap, not every address. Use an
     * address hl itself just vacated — attach with addr=0, note where it
     * landed, detach (which removes the region), then re-attach a second
     * segment at that exact explicit address. */
    TEST("explicit shmaddr at a freed SHM address still attaches");
    {
        char *first = shmat(id, NULL, 0);   /* let hl choose */
        if (first == (char *)-1) { FAILF("shmat(addr=0): %s", strerror(errno)); }
        else {
            uintptr_t freed = (uintptr_t)first;
            shmdt(first);                    /* region removed → addr now free */
            int id2 = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT | 0600);
            errno = 0;
            void *r = (id2 >= 0) ? shmat(id2, (void *)freed, 0) : (void *)-1;
            if (id2 < 0) FAILF("shmget: %s", strerror(errno));
            else if (r == (void *)-1)
                FAILF("shmat at freed addr 0x%lx: %s", freed, strerror(errno));
            else if ((uintptr_t)r != freed)
                FAILF("attached at %p, asked for 0x%lx", r, freed);
            else {
                memcpy(r, "FREEADDR", 9);
                if (memcmp(r, "FREEADDR", 9) == 0) PASS();
                else FAIL("readback mismatch");
                shmdt(r);
            }
            if (id2 >= 0) shmctl(id2, IPC_RMID, NULL);
        }
    }

    /* (H2) A guest mprotect on an attached window splits its 2MB block into
     * an L3 table. Detach must COLLAPSE that split (free the L3 page, clear
     * the L2 slot); otherwise the slot reads "mapped" forever and every
     * later shmat of ANY segment fails EINVAL for the process lifetime. */
    TEST("mprotect on a window does not permanently disable SHM");
    {
        char *p = shmat(id, NULL, 0);
        if (p == (char *)-1) { FAILF("shmat: %s", strerror(errno)); }
        else {
            *p = 'A';
            if (mprotect(p, 4096, PROT_READ) != 0) FAILF("mprotect: %s", strerror(errno));
            else if (shmdt(p) != 0) FAILF("shmdt: %s", strerror(errno));
            else {
                /* same id must re-attach */
                char *q = shmat(id, NULL, 0);
                /* a DIFFERENT segment must also still attach */
                int id2 = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT | 0600);
                char *w = (id2 >= 0) ? shmat(id2, NULL, 0) : (char *)-1;
                if (q == (char *)-1)
                    FAILF("re-attach same id failed: %s", strerror(errno));
                else if (w == (char *)-1)
                    FAILF("attach different id failed: %s", strerror(errno));
                else {
                    *q = 'B'; *w = 'C';   /* both windows usable */
                    PASS();
                }
                if (q != (char *)-1) shmdt(q);
                if (w != (char *)-1) shmdt(w);
                if (id2 >= 0) shmctl(id2, IPC_RMID, NULL);
            }
        }
    }

    /* (H2) Repeated attach → partial-mprotect → detach must keep working.
     * (Pool RECLAMATION itself is asserted host-side in test-diagnostics.sh,
     * because exhaustion degrades gracefully in-guest and is not observable
     * here — only the "pool exhausted" stderr is.) */
    TEST("attach/mprotect/detach cycles keep succeeding");
    {
        int ok = 1;
        for (int i = 0; i < 300 && ok; i++) {
            char *p = shmat(id, NULL, 0);
            if (p == (char *)-1) {
                FAILF("cycle %d shmat: %s (PT pool exhausted?)", i, strerror(errno));
                ok = 0; break;
            }
            *p = (char)i;
            if (mprotect(p, 4096, PROT_READ) != 0) { FAILF("cycle %d mprotect: %s", i, strerror(errno)); ok = 0; break; }
            if (shmdt(p) != 0) { FAILF("cycle %d shmdt: %s", i, strerror(errno)); ok = 0; break; }
        }
        if (ok) PASS();
    }

    /* (H2/SHM-F3) After detaching a split window, the VA must be gone —
     * accessing it faults, it does not silently alias primary RAM. */
    TEST("a detached (split) window is no longer accessible");
    {
        char *p = shmat(id, NULL, 0);
        if (p == (char *)-1) { FAILF("shmat: %s", strerror(errno)); }
        else {
            *p = 'Z';
            mprotect(p, 4096, PROT_READ);   /* split */
            shmdt(p);
            /* Reading *p now must fault (SIGSEGV), caught below. */
            pid_t pid = fork();
            if (pid == 0) {
                volatile char c = *(volatile char *)p;  /* expect SIGSEGV */
                _exit((c == 'Z') ? 42 : 43);             /* reached only if no fault */
            }
            int st = 0; waitpid(pid, &st, 0);
            /* The read must NOT succeed (42/43). It faults; hl may report a
             * guest signal-death either as WIFSIGNALED or as an encoded
             * exit code (128 + signo). Either means "no stale alias". */
            int read_ok = WIFEXITED(st) &&
                          (WEXITSTATUS(st) == 42 || WEXITSTATUS(st) == 43);
            if (read_ok)
                FAIL("detached window still readable — stale alias onto RAM");
            else PASS();
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
