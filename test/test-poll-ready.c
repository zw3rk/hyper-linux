/* test-poll-ready.c — an already-ready poll/select must not return EINTR
 *
 * Regression (H4/V10): hl parks indefinite poll/select on the host with an
 * internal wakeup-pipe fd. When a fd is ALREADY ready — a closed fd pre-set
 * to POLLNVAL, or an always-ready device like /dev/mixer (the OSS mixer
 * XMMS polls) — poll(2) returns immediately and must report that result.
 * The round-2 wake handling instead saw the invisible wake byte, decremented
 * the count to zero, and returned EINTR, swallowing the POLLNVAL / ready
 * result.
 *
 * The loop mirrors the field pattern: a wake byte is planted (a child exit),
 * and on the buggy build the poll returns EINTR, whose handler re-plants —
 * sustaining EINTR every iteration. On the fixed build the very first poll
 * reports POLLNVAL/ready (draining the wake byte via the force_immediate
 * fall-through), and no EINTR is ever produced.
 *
 * The complementary (+) case — an indefinite wait with NOTHING ready must
 * still surface a wake as EINTR — is covered by test-poll-wakeup.
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
#include <poll.h>
#include <sys/select.h>
#include <sys/wait.h>

static void plant_wake(void) {
    pid_t p = fork();
    if (p == 0) _exit(0);
    if (p > 0) waitpid(p, NULL, 0);
    usleep(20000);
}

int main(void) {
    int passes = 0, fails = 0;
    printf("test-poll-ready: an already-ready wait must not return EINTR\n");

    /* ---- poll: closed fd is POLLNVAL, not EINTR ---- */
    TEST("poll(-1) on a closed fd returns POLLNVAL under wake churn");
    {
        plant_wake();
        int nval = 0, eintr = 0, other = 0;
        for (int i = 0; i < 20; i++) {
            struct pollfd pf = { .fd = 987, .events = POLLIN }; /* never opened */
            int r = poll(&pf, 1, -1);
            if (r < 0 && errno == EINTR) { eintr++; plant_wake(); }
            else if (r > 0 && (pf.revents & POLLNVAL)) nval++;
            else other++;
        }
        if (eintr == 0 && nval == 20) PASS();
        else FAILF("POLLNVAL=%d EINTR=%d other=%d (want 20/0/0)",
                   nval, eintr, other);
    }

    /* ---- poll: /dev/mixer is always-ready, not EINTR ---- */
    TEST("poll(-1) on /dev/mixer returns ready under wake churn");
    {
        int m = open("/dev/mixer", O_RDONLY);
        if (m < 0) { printf("(skip: no /dev/mixer: %s) ", strerror(errno)); PASS(); }
        else {
            plant_wake();
            int ready = 0, eintr = 0;
            for (int i = 0; i < 10; i++) {
                struct pollfd pf = { .fd = m, .events = POLLIN };
                int r = poll(&pf, 1, -1);
                if (r < 0 && errno == EINTR) { eintr++; plant_wake(); }
                else if (r > 0) ready++;
            }
            close(m);
            if (eintr == 0 && ready == 10) PASS();
            else FAILF("ready=%d EINTR=%d (want 10/0)", ready, eintr);
        }
    }

    /* ---- select: an always-ready fd with no host alias exercises the
     * pselect `always_count` guard (the analog of poll's force_immediate).
     * /dev/mixer is such a fd, so select must report it ready, not EINTR. ---- */
    TEST("select(NULL) on /dev/mixer returns ready under wake churn");
    {
        int m = open("/dev/mixer", O_RDONLY);
        if (m < 0) { printf("(skip: no /dev/mixer: %s) ", strerror(errno)); PASS(); }
        else {
            plant_wake();
            int ready = 0, eintr = 0;
            for (int i = 0; i < 10; i++) {
                fd_set rd; FD_ZERO(&rd); FD_SET(m, &rd);
                int r = select(m + 1, &rd, NULL, NULL, NULL);
                if (r < 0 && errno == EINTR) { eintr++; plant_wake(); }
                else if (r > 0 && FD_ISSET(m, &rd)) ready++;
            }
            close(m);
            if (eintr == 0 && ready == 10) PASS();
            else FAILF("ready=%d EINTR=%d (want 10/0)", ready, eintr);
        }
    }

    SUMMARY("test-poll-ready");
    return fails ? 1 : 0;
}
