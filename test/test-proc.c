/* test-proc.c — Test /proc and /dev emulation
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: /proc/self/cmdline, /proc/meminfo, /proc/stat, /proc/version,
 *        /proc/filesystems, /proc/mounts, readlink(/proc/self/exe),
 *        /dev/null, /dev/zero, /dev/urandom
 *
 * Syscalls exercised: openat(56), read(63), write(64), readlinkat(78),
 *                     close(57)
 */
#include "test-harness.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* Read entire file into buffer. Returns bytes read, or -1 on error. */
static ssize_t read_file(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    ssize_t total = 0;
    while ((size_t)total < bufsz - 1) {
        ssize_t n = read(fd, buf + total, bufsz - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    close(fd);
    return total;
}

int main(void) {
    int passes = 0, fails = 0;

    printf("test-proc: /proc and /dev emulation tests\n");

    /* /proc/self/cmdline: should contain argv[0], NUL-separated */
    TEST("/proc/self/cmdline");
    {
        char buf[4096];
        ssize_t n = read_file("/proc/self/cmdline", buf, sizeof(buf));
        if (n > 0) {
            /* argv[0] should be non-empty (the binary name) */
            if (strlen(buf) > 0) PASS();
            else FAIL("empty argv[0]");
        } else FAIL("read failed");
    }

    /* /proc/meminfo: should contain "MemTotal:" with a positive value */
    TEST("/proc/meminfo");
    {
        char buf[4096];
        ssize_t n = read_file("/proc/meminfo", buf, sizeof(buf));
        if (n > 0) {
            char *mt = strstr(buf, "MemTotal:");
            if (mt) {
                unsigned long kb = 0;
                sscanf(mt, "MemTotal: %lu", &kb);
                if (kb > 0) PASS();
                else FAIL("MemTotal is 0");
            } else FAIL("MemTotal not found");
        } else FAIL("read failed");
    }

    /* /proc/stat: should contain "cpu " aggregate line */
    TEST("/proc/stat");
    {
        char buf[8192];
        ssize_t n = read_file("/proc/stat", buf, sizeof(buf));
        if (n > 0) {
            if (strstr(buf, "cpu ") != NULL) PASS();
            else FAIL("cpu line not found");
        } else FAIL("read failed");
    }

    /* /proc/version: should start with "Linux version" */
    TEST("/proc/version");
    {
        char buf[512];
        ssize_t n = read_file("/proc/version", buf, sizeof(buf));
        if (n > 0) {
            if (strncmp(buf, "Linux version", 13) == 0) PASS();
            else FAIL("wrong prefix");
        } else FAIL("read failed");
    }

    /* /proc/filesystems: should contain at least one fs type */
    TEST("/proc/filesystems");
    {
        char buf[1024];
        ssize_t n = read_file("/proc/filesystems", buf, sizeof(buf));
        if (n > 0) {
            /* Check for ext4 or tmpfs (both in our synthetic file) */
            if (strstr(buf, "ext4") || strstr(buf, "tmpfs")) PASS();
            else FAIL("no known fs type");
        } else FAIL("read failed");
    }

    /* /proc/mounts: should have at least one mount entry */
    TEST("/proc/mounts");
    {
        char buf[2048];
        ssize_t n = read_file("/proc/mounts", buf, sizeof(buf));
        if (n > 0) {
            /* Check for any mount point */
            if (strstr(buf, " / ") != NULL) PASS();
            else FAIL("no root mount");
        } else FAIL("read failed");
    }

    /* readlink("/proc/self/exe"): should return a non-empty path */
    TEST("readlink /proc/self/exe");
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (strlen(buf) > 0) PASS();
            else FAIL("empty path");
        } else FAIL("readlink failed");
    }

    /* /dev/null: write succeeds, read returns 0 (EOF) */
    TEST("/dev/null write+read");
    {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            char data[] = "test";
            ssize_t w = write(fd, data, 4);
            if (w == 4) {
                char buf[16];
                ssize_t r = read(fd, buf, sizeof(buf));
                if (r == 0) PASS();
                else FAIL("read not EOF");
            } else FAIL("write failed");
            close(fd);
        } else FAIL("open failed");
    }

    /* /dev/zero: read returns all zeros */
    TEST("/dev/zero");
    {
        int fd = open("/dev/zero", O_RDONLY);
        if (fd >= 0) {
            unsigned char buf[64];
            memset(buf, 0xFF, sizeof(buf));
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r == 64) {
                int all_zero = 1;
                for (int i = 0; i < 64; i++) {
                    if (buf[i] != 0) { all_zero = 0; break; }
                }
                if (all_zero) PASS();
                else FAIL("not all zeros");
            } else FAIL("read wrong size");
            close(fd);
        } else FAIL("open failed");
    }

    /* /dev/urandom: read returns data (extremely unlikely to be all zeros) */
    TEST("/dev/urandom");
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            unsigned char buf[32];
            memset(buf, 0, sizeof(buf));
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r == 32) {
                int any_nonzero = 0;
                for (int i = 0; i < 32; i++) {
                    if (buf[i] != 0) { any_nonzero = 1; break; }
                }
                if (any_nonzero) PASS();
                else FAIL("all zeros (astronomically unlikely)");
            } else FAIL("read wrong size");
            close(fd);
        } else FAIL("open failed");
    }

    SUMMARY("test-proc");
    return fails > 0 ? 1 : 0;
}
