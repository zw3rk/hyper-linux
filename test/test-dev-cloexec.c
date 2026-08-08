/* test-dev-cloexec.c — a CLOEXEC /dev fd must not abort hl on execve
 *
 * Regression (H3/V5): the exec CLOEXEC sweep freed every non-FD_DIR `.dir`
 * with a blind `free()`. A pseudo /dev node (FD_DEVICE) stores a pointer
 * into the STATIC device registry there, so a DIRECT execve while holding
 * an O_CLOEXEC /dev fd did `free(&g_nodes[i])` → macOS "pointer being freed
 * was not allocated" → SIGABRT (hl exit 134). `open("/dev/null",O_CLOEXEC)`
 * then `exec` is a ubiquitous pattern (shell `exec`, self-re-exec).
 *
 * The crash requires a DIRECT execve — the sweep runs in the process that
 * holds the fd. (A fork+exec child does not inherit the FD_DEVICE `.dir`, so
 * it does not reproduce; that is why an earlier fork-based version of this
 * test passed even against the bug.) So this test execs itself directly and
 * lets the result travel through hl's own exit code: the re-exec'd child
 * prints the verdict. If hl aborts in the sweep, there is no child and hl
 * exits 134 — the Makefile's run_test sees the non-zero exit and fails.
 *
 * (+) It opens a regular file and a directory O_CLOEXEC too, so the same
 * exec must still closedir/free those correctly (the whitelist must spare
 * FD_DEVICE without breaking the kinds it should release).
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
#include <sys/stat.h>

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 1 && strcmp(argv[1], "reexec") == 0) {
        /* Reached only if the parent's DIRECT execve did not abort hl. */
        int passes = 0, fails = 0;
        printf("test-dev-cloexec: CLOEXEC /dev fd survived execve\n");
        TEST("execve past O_CLOEXEC /dev + regular + dir fds did not abort hl");
        PASS();
        SUMMARY("test-dev-cloexec");
        return fails ? 1 : 0;
    }

    /* Parent: hold a spread of CLOEXEC fds whose `.dir` the exec sweep will
     * process — the /dev nodes (FD_DEVICE, must NOT be freed), a regular
     * file (no .dir), and a directory (FD_DIR, must closedir). Then execve
     * DIRECTLY so the sweep runs in this process. */
    const char *devs[] = { "/dev/null", "/dev/zero", "/dev/urandom" };
    for (size_t i = 0; i < sizeof(devs)/sizeof(devs[0]); i++) {
        int fd = open(devs[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            printf("test-dev-cloexec: open(%s): %s\n", devs[i], strerror(errno));
            return 1;
        }
    }
    int rf = open("cloexec-plain", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    (void)rf;
    mkdir("cloexec-dir", 0755);
    int df = open("cloexec-dir", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    (void)df;

    char *av[] = { argv[0], "reexec", NULL };
    execv(argv[0], av);
    printf("test-dev-cloexec: execv failed: %s\n", strerror(errno));
    return 1;
}
