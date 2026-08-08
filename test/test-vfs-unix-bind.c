/* test-vfs-unix-bind.c — a bind must confine AF_UNIX bind/connect (VFS-F2)
 *
 * Regression: in rooted mode sys_bind/sys_connect named the RAW guest sun_path
 * (bind/connect on the absolute host string), so a confined guest could create
 * or reach an AF_UNIX socket at a host path outside its bind (e.g. host /tmp).
 * The fix resolves the sun_path beneath the bind root and performs the
 * bind/connect against a per-thread cwd pinned to the resolved parent, using a
 * RELATIVE leaf — race-free, and any out-of-bind path is refused.
 *
 * Run with (--isolated so host /tmp is NOT auto-bound and stays a writable
 * out-of-bind escape target):
 *   hl --fs-mode=rooted --isolated --bind <tmp>:/home/user --guest-cwd /home/user
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int passes = 0, fails = 0;

static socklen_t fill_un(struct sockaddr_un *un, const char *path) {
    memset(un, 0, sizeof(*un));
    un->sun_family = AF_UNIX;
    snprintf(un->sun_path, sizeof(un->sun_path), "%s", path);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                       strlen(un->sun_path) + 1);
}

/* A bind that must NOT succeed: a return of 0 means the guest created a socket
 * at a host path outside its bind. A macro so PASS()/FAIL() touch main's
 * counters. Require a containment errno, not just any failure. */
#define MUST_NOT_BIND(what, path)                                          \
    do {                                                                   \
        TEST(what);                                                        \
        int _s = socket(AF_UNIX, SOCK_STREAM, 0);                          \
        struct sockaddr_un _un; socklen_t _l = fill_un(&_un, (path));      \
        errno = 0;                                                         \
        int _r = bind(_s, (struct sockaddr *)&_un, _l);                    \
        int _e = errno;                                                    \
        if (_r == 0) { unlink((path)); FAILF("ESCAPED: bound %s", (path)); }\
        else if (_e == EACCES || _e == ENOENT || _e == EPERM ||            \
                 _e == EROFS || _e == ELOOP || _e == ENOTDIR) PASS();      \
        else FAILF("failed with %s, not a containment refusal",            \
                   strerror(_e));                                          \
        close(_s);                                                         \
    } while (0)

int main(void) {
    printf("test-vfs-unix-bind: AF_UNIX bind/connect must stay in the bind\n");

    /* --isolated disables implicit binds, not the guest environment for an
     * explicit /home/user bind supplied by the caller. */
    TEST("explicit isolated home bind sets guest HOME");
    {
        const char *home = getenv("HOME");
        if (home && strcmp(home, "/home/user") == 0) PASS();
        else FAILF("HOME=%s", home ? home : "(unset)");
    }

    /* Control (+): bind an AF_UNIX socket INSIDE the bind (CWD /home/user),
     * then connect a client to it — proves the confined bind+connect path
     * works end to end, otherwise the refusals below prove nothing. */
    const char *inpath = "srv.sock";
    unlink(inpath);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST("control: bind an AF_UNIX socket inside the bind");
    {
        struct sockaddr_un su; socklen_t sl = fill_un(&su, inpath);
        if (bind(srv, (struct sockaddr *)&su, sl) != 0) {
            FAILF("bind: %s", strerror(errno));
        } else {
            PASS();
            TEST("in-bind connect reaches the in-bind socket");
            if (listen(srv, 1) != 0) {
                FAILF("listen: %s", strerror(errno));
            } else {
                int cli = socket(AF_UNIX, SOCK_STREAM, 0);
                struct sockaddr_un cu; socklen_t cl = fill_un(&cu, inpath);
                if (connect(cli, (struct sockaddr *)&cu, cl) == 0) PASS();
                else FAILF("connect: %s", strerror(errno));
                close(cli);
            }
        }
    }
    close(srv);
    unlink(inpath);

    /* (−) The escape: an absolute host path outside every bind. Under
     * --isolated host /tmp is NOT bound and is writable, so under the bug the
     * bind created a real socket there (a confinement escape). */
    char esc[128];
    snprintf(esc, sizeof(esc), "/tmp/hl-vfs-escape-%d.sock", (int)getpid());
    unlink(esc);
    MUST_NOT_BIND("bind escapes to host /tmp", esc);
    MUST_NOT_BIND("bind escapes to absolute /etc", "/etc/hl-vfs-escape.sock");

    /* Connect to an out-of-bind path must be refused before it reaches the
     * host (co-assertion: shares unix_confine_begin with the bind path). */
    TEST("connect to an out-of-bind path is refused");
    {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un un; socklen_t l = fill_un(&un, esc);
        errno = 0;
        int r = connect(s, (struct sockaddr *)&un, l);
        int e = errno;
        if (r == 0) FAILF("ESCAPED: connected %s", esc);
        else if (e == EACCES || e == ENOENT || e == EPERM ||
                 e == ECONNREFUSED || e == ELOOP || e == ENOTDIR) PASS();
        else FAILF("failed with %s", strerror(e));
        close(s);
    }

    SUMMARY("test-vfs-unix-bind");
    return fails ? 1 : 0;
}
