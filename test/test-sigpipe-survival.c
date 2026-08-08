/* test-sigpipe-survival.c — a broken pipe must not kill the VM
 *
 * Regression: hl never set SIG_IGN for host SIGPIPE, so the host write()
 * inside sys_write was killed by SIGPIPE before it could return EPIPE.
 * The whole VM died with exit 141 and the guest-side SIGPIPE emulation
 * (signal_queue(LINUX_SIGPIPE) on EPIPE) was unreachable.
 *
 * This test only produces output at all if the host survived, so a
 * regression shows up as a hard failure rather than a silent skip.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t got_sigpipe = 0;
static void on_sigpipe(int sig) { (void)sig; got_sigpipe = 1; }

int main(void) {
    int passes = 0, fails = 0;
    printf("test-sigpipe-survival: broken pipe must not kill the VM\n");

    /* (-) case: with SIGPIPE ignored, write() to a broken pipe must
     * report EPIPE rather than terminating the process. */
    TEST("write to broken pipe returns EPIPE (SIG_IGN)");
    {
        signal(SIGPIPE, SIG_IGN);
        int pfd[2];
        if (pipe(pfd) < 0) FAIL("pipe");
        else {
            close(pfd[0]);                    /* break it */
            errno = 0;
            ssize_t w = write(pfd[1], "x", 1);
            if (w < 0 && errno == EPIPE) PASS();
            else FAILF("expected -1/EPIPE, got rc=%zd errno=%d", w, errno);
            close(pfd[1]);
        }
    }

    /* (+) case: with a handler installed, the guest must actually receive
     * SIGPIPE — proving delivery is emulated, not just suppressed. */
    TEST("handler receives guest SIGPIPE");
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_sigpipe;
        sigaction(SIGPIPE, &sa, NULL);

        int pfd[2];
        if (pipe(pfd) < 0) FAIL("pipe");
        else {
            close(pfd[0]);
            got_sigpipe = 0;
            errno = 0;
            ssize_t w = write(pfd[1], "x", 1);
            if (w < 0 && errno == EPIPE && got_sigpipe) PASS();
            else FAILF("rc=%zd errno=%d got_sigpipe=%d",
                      w, errno, (int)got_sigpipe);
            close(pfd[1]);
        }
    }

    /* The VM is still alive here — that is the headline assertion. */
    TEST("VM still alive after broken-pipe writes");
    PASS();

    SUMMARY("test-sigpipe-survival");
    return fails ? 1 : 0;
}
