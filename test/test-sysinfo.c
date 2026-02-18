/* test-sysinfo.c — Test process/system info syscalls (Batch 2)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: sysinfo, getrusage, getgroups, prlimit64, getppid, sched_getaffinity
 */
#define _GNU_SOURCE  /* Required for cpu_set_t, CPU_ZERO, CPU_COUNT on musl */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <sched.h>

#define TEST(name) printf("  %-30s ", name)
#define PASS()     do { printf("OK\n"); passes++; } while(0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); fails++; } while(0)

int main(void) {
    int passes = 0, fails = 0;

    printf("test-sysinfo: Batch 2 process/system info tests\n");

    /* Test sysinfo */
    TEST("sysinfo");
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            if (si.uptime > 0 && si.totalram > 0 && si.mem_unit > 0)
                PASS();
            else
                FAIL("invalid values");
        } else FAIL("sysinfo failed");
    }

    /* Test getrusage */
    TEST("getrusage");
    {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            /* Just verify it doesn't crash and returns sensible data */
            if (usage.ru_maxrss >= 0) PASS();
            else FAIL("invalid maxrss");
        } else FAIL("getrusage failed");
    }

    /* Test getgroups */
    TEST("getgroups");
    {
        int ngroups = getgroups(0, NULL);
        if (ngroups >= 0) {
            /* Verify we can also read the actual groups */
            if (ngroups > 0) {
                gid_t groups[64];
                int n = getgroups(ngroups > 64 ? 64 : ngroups, groups);
                if (n >= 0) PASS();
                else FAIL("getgroups read failed");
            } else PASS();
        } else FAIL("getgroups count failed");
    }

    /* Test prlimit64 (via getrlimit/setrlimit) */
    TEST("prlimit64 (RLIMIT_NOFILE)");
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            if (rl.rlim_cur > 0) PASS();
            else FAIL("invalid rlim_cur");
        } else FAIL("getrlimit failed");
    }

    /* Test getppid */
    TEST("getppid");
    {
        pid_t ppid = getppid();
        /* In our single-process model, ppid should be 0 */
        if (ppid >= 0) PASS();
        else FAIL("getppid returned negative");
    }

    /* Test sched_getaffinity */
    TEST("sched_getaffinity");
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) == 0) {
            /* Should have at least 1 CPU */
            int count = CPU_COUNT(&set);
            if (count >= 1) PASS();
            else FAIL("no CPUs in affinity mask");
        } else FAIL("sched_getaffinity failed");
    }

    /* Test getpid / gettid */
    TEST("getpid");
    {
        pid_t pid = getpid();
        if (pid > 0) PASS();
        else FAIL("invalid pid");
    }

    printf("\nResults: %d passed, %d failed\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
