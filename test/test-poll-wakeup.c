/* test-poll-wakeup.c — an indefinite wait must never return 0
 *
 * hl parks indefinite poll/select/epoll_wait on the host with an extra
 * internal "wakeup pipe" fd, so exit_group and fork bookkeeping can break
 * threads out. That fd is invisible to the guest, so when it fires the
 * event is subtracted from the count — which used to leave the syscall
 * returning 0 from a wait the guest asked to be indefinite.
 *
 * (-) poll(fds, n, -1) returning 0 is a protocol violation. Worse, it is
 *     self-sustaining: wakeup_pipe_signal() writes one byte per registered
 *     waiter, every byte no one consumed keeps the pipe readable, and the
 *     next indefinite wait therefore returns 0 immediately. The reported
 *     symptom was ~20,000 spurious returns/second.
 *
 * (-) epoll_pwait never registered itself in poll_waiters at all, so the
 *     broadcast was sized from the poll/select waiters alone and threads
 *     parked in epoll simply missed it; it also drained a byte on EVERY
 *     return, whether or not the wake had fired, stealing wakes meant for
 *     other threads.
 *
 * (+) A real event must still wake the waiter and be reported exactly once.
 *
 * Each waiter here blocks on a pipe that stays empty, while the main thread
 * forks a child (fork/exit is what signals the wakeup pipe). The waiter is
 * released at the end by real data.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <poll.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <sys/wait.h>

typedef struct {
    int   fd;           /* pipe read end, stays empty until release */
    int   zero_returns; /* indefinite wait returned 0 — the defect */
    int   data_seen;    /* the real event arrived */
    int   eintrs;       /* EINTR returns — one nudge per worker exit is
                         * legitimate; an unbounded stream is the bug */
    int   err;          /* unexpected failure */
} waiter_t;

static void *poll_waiter(void *arg) {
    waiter_t *w = arg;
    for (;;) {
        struct pollfd pfd = { .fd = w->fd, .events = POLLIN };
        int r = poll(&pfd, 1, -1);
        if (r == 0)      { w->zero_returns++; continue; }
        if (r < 0)       { if (errno == EINTR) { w->eintrs++; continue; }
                           w->err = errno; return NULL; }
        if (pfd.revents & POLLIN) { w->data_seen = 1; return NULL; }
    }
}

static void *select_waiter(void *arg) {
    waiter_t *w = arg;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(w->fd, &rd);
        int r = select(w->fd + 1, &rd, NULL, NULL, NULL);
        if (r == 0)      { w->zero_returns++; continue; }
        if (r < 0)       { if (errno == EINTR) { w->eintrs++; continue; }
                           w->err = errno; return NULL; }
        if (FD_ISSET(w->fd, &rd)) { w->data_seen = 1; return NULL; }
    }
}

static void *epoll_waiter(void *arg) {
    waiter_t *w = arg;
    int ep = epoll_create1(0);
    if (ep < 0) { w->err = errno; return NULL; }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = w->fd };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, w->fd, &ev) != 0) {
        w->err = errno; close(ep); return NULL;
    }
    for (;;) {
        struct epoll_event out[4];
        int r = epoll_wait(ep, out, 4, -1);
        if (r == 0)      { w->zero_returns++; continue; }
        if (r < 0)       { if (errno == EINTR) { w->eintrs++; continue; }
                           w->err = errno; break; }
        w->data_seen = 1;
        break;
    }
    close(ep);
    return NULL;
}

/* Fork and reap a trivial child a few times: that is what drives
 * wakeup_pipe_signal() in hl. */
static void churn_wakeups(int n) {
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) _exit(0);
        if (pid > 0) waitpid(pid, NULL, 0);
        usleep(20000);
    }
}

static int run_case(const char *name, void *(*fn)(void *),
                    int *passes, int *fails) {
    int p[2];
    if (pipe(p) != 0) { printf("  %-46s FAIL: pipe\n", name); (*fails)++; return 1; }

    waiter_t w = { .fd = p[0] };
    pthread_t th;
    if (pthread_create(&th, NULL, fn, &w) != 0) {
        printf("  %-46s FAIL: pthread_create\n", name);
        (*fails)++; close(p[0]); close(p[1]); return 1;
    }

    usleep(100000);        /* let the waiter park */
    churn_wakeups(3);      /* signal the wakeup pipe while it is parked */

    int spurious = w.zero_returns;   /* sample before releasing */
    int eintrs   = w.eintrs;

    char c = 'x';
    ssize_t wr = write(p[1], &c, 1); /* the real event */
    (void)wr;
    pthread_join(th, NULL);

    printf("  %-46s ", name);
    if (w.err) {
        printf("FAIL: %s\n", strerror(w.err));
        (*fails)++;
    } else if (spurious > 0) {
        printf("FAIL: indefinite wait returned 0 %d time(s)\n", spurious);
        (*fails)++;
    } else if (eintrs > 8) {
        /* One nudge per worker exit is by design; a stream of them means
         * the one-shot flag is never consumed and the caller busy-spins. */
        printf("FAIL: %d EINTRs — the interrupt flag is never cleared\n",
               eintrs);
        (*fails)++;
    } else if (!w.data_seen) {
        printf("FAIL: the real event was never reported\n");
        (*fails)++;
    } else {
        printf("OK\n");
        (*passes)++;
    }
    close(p[0]); close(p[1]);
    return 0;
}

int main(void) {
    int passes = 0, fails = 0;
    /* Unbuffered: this test forks, and a buffered stream would be
     * duplicated into the child. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("test-poll-wakeup: indefinite waits never return 0\n");

    run_case("poll(-1) survives a wakeup-pipe signal",   poll_waiter,   &passes, &fails);
    run_case("select(NULL) survives a wakeup-pipe signal", select_waiter, &passes, &fails);
    run_case("epoll_wait(-1) survives a wakeup-pipe signal", epoll_waiter, &passes, &fails);

    printf("\ntest-poll-wakeup: %d passed, %d failed — %s\n",
           passes, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
