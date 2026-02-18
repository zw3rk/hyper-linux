/* test-complex.c — exercise multiple syscalls
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: uname, malloc (brk/mmap), clock_gettime, getpid, getcwd.
 * Returns exit code 42 to verify non-zero exit propagation. */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname uts;
    if (uname(&uts) == 0) {
        printf("System: %s %s %s %s\n", uts.sysname, uts.nodename,
               uts.release, uts.machine);
    }

    char *p = malloc(1024);
    if (p) {
        printf("Hello from malloc!\n");
        free(p);
    }

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        printf("Clock: %ld.%09ld\n", (long)ts.tv_sec, ts.tv_nsec);
    }

    printf("PID: %d\n", getpid());

    char cwd[256];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("CWD: %s\n", cwd);
    }

    return 42;
}
