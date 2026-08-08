/* test-abstract-unix.c — Linux abstract AF_UNIX sockets
 *
 * macOS has no abstract namespace, so hl maps sun_path[0]=='\0' names onto
 * files in a private directory. Two regressions in that mapping:
 *
 * (-) The directory was the fixed path /tmp/.hl-abstract. /tmp is
 *     world-writable and sticky, so any local user could pre-create it (or
 *     a listening socket inside it); mkdir() then failed with EEXIST and hl
 *     used it anyway, putting guest AF_UNIX traffic inside a directory
 *     someone else controls. The same fixed path also meant whoever created
 *     it owned it 0700 — every other user on the machine got EACCES for
 *     good. It now resolves under the per-user TMPDIR and is refused unless
 *     it is a directory owned by this uid with no group/other write.
 *
 * (-) A Linux abstract name has no filesystem lifetime: it disappears with
 *     its last reference. The stand-in file did not, so a socket left by an
 *     earlier run made every later bind fail with EADDRINUSE. bind() now
 *     clears a stale socket at the mapped path first.
 *
 * (+) bind/listen/connect/accept over an abstract name must work, and the
 *     name must stay distinct from the same string used as a real path.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>   /* offsetof */
#include <sys/socket.h>
#include <sys/un.h>

/* Build a Linux abstract sockaddr_un: sun_path[0] == '\0', then the name.
 * The length covers exactly family + NUL + name, with no trailing NUL. */
static socklen_t abstract_addr(struct sockaddr_un *sa, const char *name) {
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    size_t n = strlen(name);
    if (n > sizeof(sa->sun_path) - 2) n = sizeof(sa->sun_path) - 2;
    sa->sun_path[0] = '\0';
    memcpy(sa->sun_path + 1, name, n);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

/* One bind/listen/connect/accept round trip. Returns 0 on success. */
static int round_trip(const char *name, char *why, size_t why_sz) {
    struct sockaddr_un sa;
    socklen_t len = abstract_addr(&sa, name);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { snprintf(why, why_sz, "socket: %s", strerror(errno)); return -1; }
    if (bind(srv, (struct sockaddr *)&sa, len) != 0) {
        snprintf(why, why_sz, "bind: %s", strerror(errno));
        close(srv); return -1;
    }
    if (listen(srv, 4) != 0) {
        snprintf(why, why_sz, "listen: %s", strerror(errno));
        close(srv); return -1;
    }

    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cli < 0) { snprintf(why, why_sz, "socket2: %s", strerror(errno));
                   close(srv); return -1; }
    if (connect(cli, (struct sockaddr *)&sa, len) != 0) {
        snprintf(why, why_sz, "connect: %s", strerror(errno));
        close(cli); close(srv); return -1;
    }
    int acc = accept(srv, NULL, NULL);
    if (acc < 0) { snprintf(why, why_sz, "accept: %s", strerror(errno));
                   close(cli); close(srv); return -1; }

    ssize_t w = write(cli, "PING", 4);
    char buf[8] = {0};
    ssize_t r = read(acc, buf, sizeof(buf));
    int ok = (w == 4 && r == 4 && memcmp(buf, "PING", 4) == 0);
    if (!ok) snprintf(why, why_sz, "echo: wrote %zd read %zd (%.4s)", w, r, buf);

    close(acc); close(cli); close(srv);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    int passes = 0, fails = 0;
    char why[128] = {0};

    /* "unsafe" mode: the harness has pre-created hl's abstract-socket
     * directory group/other-writable, standing in for a local attacker who
     * squats the path before hl starts. hl must refuse to use it — the old
     * fixed /tmp/.hl-abstract took EEXIST as success and put guest AF_UNIX
     * traffic inside a directory someone else controls. The refusal is not
     * visible from the guest as anything but a failed bind, which is
     * exactly the point: better to fail than to be redirected. */
    if (argc > 1 && strcmp(argv[1], "unsafe") == 0) {
        printf("test-abstract-unix: refuses an unsafe abstract dir\n");
        TEST("bind fails when the abstract dir is not private");
        if (round_trip("hl-test-abstract-x", why, sizeof(why)) != 0) PASS();
        else FAIL("bound anyway — traffic went into an attacker's directory");
        SUMMARY("test-abstract-unix");
        return fails ? 1 : 0;
    }

    printf("test-abstract-unix: abstract AF_UNIX namespace\n");

    TEST("bind/connect over an abstract name");
    if (round_trip("hl-test-abstract-a", why, sizeof(why)) == 0) PASS();
    else FAILF("%s", why);

    /* (-) The mapped file outlived the socket, so a second bind of the same
     * abstract name failed with EADDRINUSE. On Linux the name is gone the
     * moment the last reference closes, so this must simply work. */
    TEST("the same abstract name can be bound again after close");
    if (round_trip("hl-test-abstract-a", why, sizeof(why)) == 0) PASS();
    else FAILF("%s", why);

    TEST("a third bind of the same name still succeeds");
    if (round_trip("hl-test-abstract-a", why, sizeof(why)) == 0) PASS();
    else FAILF("%s", why);

    TEST("distinct abstract names do not collide");
    {
        struct sockaddr_un sa1, sa2;
        socklen_t l1 = abstract_addr(&sa1, "hl-test-abstract-b");
        socklen_t l2 = abstract_addr(&sa2, "hl-test-abstract-c");
        int s1 = socket(AF_UNIX, SOCK_STREAM, 0);
        int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
        int r1 = bind(s1, (struct sockaddr *)&sa1, l1);
        int r2 = bind(s2, (struct sockaddr *)&sa2, l2);
        if (r1 != 0 || r2 != 0)
            FAILF("bind b=%d c=%d: %s", r1, r2, strerror(errno));
        else PASS();
        close(s1); close(s2);
    }

    /* (-) The abstract name "x" and the filesystem path "x" are different
     * namespaces on Linux; the mapping must not merge them. */
    TEST("an abstract name is not the same as the literal path");
    {
        struct sockaddr_un abs;
        socklen_t la = abstract_addr(&abs, "hl-test-abstract-d");

        struct sockaddr_un fs;
        memset(&fs, 0, sizeof(fs));
        fs.sun_family = AF_UNIX;
        snprintf(fs.sun_path, sizeof(fs.sun_path), "hl-test-abstract-d");
        socklen_t lf = (socklen_t)sizeof(fs);

        unlink(fs.sun_path);
        int sa_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        int sf_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        int ra = bind(sa_fd, (struct sockaddr *)&abs, la);
        int rf = bind(sf_fd, (struct sockaddr *)&fs, lf);
        if (ra != 0) FAILF("abstract bind: %s", strerror(errno));
        else if (rf != 0) FAILF("path bind: %s", strerror(errno));
        else PASS();
        close(sa_fd); close(sf_fd);
        unlink(fs.sun_path);
    }

    SUMMARY("test-abstract-unix");
    return fails ? 1 : 0;
}
