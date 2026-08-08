/* test-tgkill-target.c — directed (per-thread) signal delivery for hl
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for the `xmms file.mp3` startup deadlock.
 *
 * glibc implements setuid()/setgid()/setreuid()/… as __nptl_setxid(): it
 * signals *every other thread* with SIGSETXID via tgkill(2) and then blocks
 * in futex_wait until each signalled thread's handler acknowledges. The
 * handler (nptl/nptl_setxid.c) starts with
 *
 *     if (si->si_code != SI_TKILL) return;
 *
 * and must run *on the signalled thread*. XMMS calls setuid(geteuid()) in
 * xmms_connect_to_session() (libxmms/xmmsctrl.c), so any control-socket
 * round-trip during startup — which is what `xmms file.mp3`, `xmms -p` and
 * `xmms -e file` do before gtk_widget_show() — hit this path.
 *
 * hl used to (a) queue tgkill signals process-wide rather than on the target
 * thread and (b) always stamp si_code = SI_USER. The acknowledgement
 * therefore never happened and setuid() hung forever, leaving XMMS with
 * realized-but-never-mapped windows.
 *
 * musl fails the same way for a different reason: __synccall (src/thread/
 * synccall.c) tkills each thread in turn and its handler starts with
 *
 *     if (__pthread_self()->tid != target_tid) return;
 *
 * then sem_post(&caller_sem). Delivered on the wrong thread it posts nothing
 * and __synccall blocks in sem_wait forever. So setuid() deadlocks under
 * both libcs, which subtest 5 checks end to end.
 *
 * Subtests:
 *   1. tgkill() runs the handler ON the target thread, not the sender,
 *      while the target is parked in select() with a finite timeout.
 *   2. The delivered siginfo carries si_code == SI_TKILL (-6).
 *   3. Same for a target parked in an *indefinite* select().
 *   4. tkill() (no tgid) behaves identically.
 *   5. setuid(geteuid()) returns while another thread is running — the
 *      libc-level handshake XMMS actually performs. Note this subtest is a
 *      guard, not the discriminator: musl aborts __synccall when tkill
 *      itself fails (as it did before tkill was implemented at all), so it
 *      only goes red once tkill exists but delivers to the wrong thread.
 *      Subtests 1–4 are what fail on the broken tree.
 *
 * Syscalls exercised: tgkill(131), tkill(130), gettid(178), pselect6(72),
 * setuid(146)
 */
#include "test-harness.h"
#include "raw-syscall.h"

#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

int passes = 0, fails = 0;

/* RT signal outside the ranges musl and glibc reserve for themselves. */
#define SIGX 40

#ifndef SI_TKILL
#define SI_TKILL (-6)
#endif

static volatile long worker_tid;
static volatile long handler_tid;
static volatile int  handler_si_code;
static volatile int  handler_count;
static volatile int  worker_blocks_forever;
static volatile int  worker_stop;

static void
handler(int sig, siginfo_t *si, void *ctx)
{
    (void)sig;
    (void)ctx;
    handler_tid = raw_gettid();
    handler_si_code = si ? si->si_code : 0;
    __atomic_fetch_add(&handler_count, 1, __ATOMIC_SEQ_CST);
}

/* Mirrors the XMMS control-socket thread: parked in select() while the
 * main thread performs the setxid handshake. */
static void *
worker_main(void *arg)
{
    (void)arg;
    __atomic_store_n(&worker_tid, raw_gettid(), __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&worker_stop, __ATOMIC_SEQ_CST)) {
        if (__atomic_load_n(&worker_blocks_forever, __ATOMIC_SEQ_CST)) {
            select(0, NULL, NULL, NULL, NULL);
        } else {
            struct timeval tv = { 0, 100000 };
            select(0, NULL, NULL, NULL, &tv);
        }
    }
    return NULL;
}

static volatile int setxid_done;
static volatile int setxid_ret;

/* The XMMS call, verbatim: libxmms/xmmsctrl.c:298 does geteuid()+setuid()
 * on every control-socket connect. Runs on its own thread so main can time
 * it out instead of hanging the suite when delivery is broken. */
static void *
setxid_main(void *arg)
{
    (void)arg;
    setxid_ret = setuid(geteuid());
    __atomic_store_n(&setxid_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* Wait up to `ms` for the handler to run once more than `before`. */
static int
wait_for_handler(int before, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (__atomic_load_n(&handler_count, __ATOMIC_SEQ_CST) > before)
            return 1;
        struct timespec t = { 0, 10 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    return 0;
}

static void
reset_observation(void)
{
    handler_tid = 0;
    handler_si_code = 0;
}

static void
check_directed(const char *name, long ret, int before)
{
    TEST(name);
    if (ret != 0) {
        FAIL("tgkill/tkill returned an error");
        return;
    }
    if (!wait_for_handler(before, 5000)) {
        FAIL("handler never ran (directed signal lost)");
        return;
    }
    if (handler_tid != worker_tid) {
        printf("FAIL: handler ran on tid %ld, expected worker tid %ld\n",
               handler_tid, worker_tid);
        fails++;
        return;
    }
    PASS();
}

static void
check_si_tkill(const char *name)
{
    TEST(name);
    if (handler_si_code != SI_TKILL) {
        printf("FAIL: si_code=%d, expected SI_TKILL (%d) — "
               "glibc __nptl_setxid_sighandler ignores anything else\n",
               handler_si_code, SI_TKILL);
        fails++;
        return;
    }
    PASS();
}

int
main(void)
{
    pthread_t th;
    struct sigaction sa;
    long pid = raw_getpid();
    int before;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\n=== tgkill/tkill directed signal delivery ===\n\n");

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGX, &sa, NULL) != 0) {
        printf("  sigaction(%d) failed\n", SIGX);
        return 1;
    }

    if (pthread_create(&th, NULL, worker_main, NULL) != 0) {
        printf("  pthread_create failed\n");
        return 1;
    }
    for (int i = 0; i < 500 && __atomic_load_n(&worker_tid, __ATOMIC_SEQ_CST) == 0; i++) {
        struct timespec t = { 0, 10 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    if (worker_tid == 0) {
        printf("  worker never reported its tid\n");
        return 1;
    }

    /* 1 + 2: target parked in select() with a finite timeout. */
    reset_observation();
    before = __atomic_load_n(&handler_count, __ATOMIC_SEQ_CST);
    check_directed("tgkill → target thread",
                   raw_tgkill(pid, worker_tid, SIGX), before);
    check_si_tkill("tgkill siginfo si_code=SI_TKILL");

    /* 3: target parked in an indefinite select() (needs the wakeup pipe). */
    __atomic_store_n(&worker_blocks_forever, 1, __ATOMIC_SEQ_CST);
    {
        struct timespec t = { 0, 300 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    reset_observation();
    before = __atomic_load_n(&handler_count, __ATOMIC_SEQ_CST);
    check_directed("tgkill → thread in blocking select",
                   raw_tgkill(pid, worker_tid, SIGX), before);
    __atomic_store_n(&worker_blocks_forever, 0, __ATOMIC_SEQ_CST);

    /* 4: tkill() (no tgid argument). */
    reset_observation();
    before = __atomic_load_n(&handler_count, __ATOMIC_SEQ_CST);
    check_directed("tkill → target thread",
                   raw_syscall2(__NR_tkill, worker_tid, SIGX), before);

    /* 5: the libc handshake itself. The worker thread above is still alive,
     * so libc takes the multi-threaded path (musl __synccall / glibc
     * __nptl_setxid) instead of the single-threaded shortcut. */
    TEST("setuid() completes with a second thread running");
    {
        pthread_t sx;
        int waited = 0;

        if (pthread_create(&sx, NULL, setxid_main, NULL) != 0) {
            FAIL("pthread_create failed");
        } else {
            while (!__atomic_load_n(&setxid_done, __ATOMIC_SEQ_CST) &&
                   waited < 5000) {
                struct timespec t = { 0, 10 * 1000 * 1000 };
                nanosleep(&t, NULL);
                waited += 10;
            }
            if (!__atomic_load_n(&setxid_done, __ATOMIC_SEQ_CST))
                FAIL("setuid() never returned — the setxid handshake "
                     "deadlocked (this is the `xmms file.mp3` hang)");
            else
                PASS();   /* the return value is irrelevant; not hanging is
                           * the property under test */
        }
    }

    /* Do not join: if directed delivery is broken the worker can still be
     * parked in an interruptible-but-uninterrupted select(). The failures
     * are already recorded; exiting must not turn them into a timeout. */
    __atomic_store_n(&worker_stop, 1, __ATOMIC_SEQ_CST);
    (void)th;

    SUMMARY("test-tgkill-target");
    return fails == 0 ? 0 : 1;
}
