/* test-file-ops.c — Test file manipulation syscalls (Batch 1)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: chmod, symlink, hardlink, utimensat, fchown, mknodat
 */
#include "test-harness.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

int main(void) {
    int passes = 0, fails = 0;
    const char *testfile = "/tmp/hl-test-file-ops.txt";
    const char *symlink_path = "/tmp/hl-test-symlink";
    const char *hardlink_path = "/tmp/hl-test-hardlink";

    printf("test-file-ops: Batch 1 file manipulation tests\n");

    /* Clean up any leftover files */
    unlink(testfile);
    unlink(symlink_path);
    unlink(hardlink_path);

    /* Create a test file */
    int fd = open(testfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { printf("FATAL: cannot create %s\n", testfile); return 1; }
    write(fd, "hello\n", 6);
    close(fd);

    /* Test fchmod */
    TEST("fchmod");
    fd = open(testfile, O_RDONLY);
    if (fd >= 0) {
        if (fchmod(fd, 0755) == 0) {
            struct stat st;
            fstat(fd, &st);
            if ((st.st_mode & 0777) == 0755) PASS();
            else FAIL("mode mismatch");
        } else FAIL("fchmod failed");
        close(fd);
    } else FAIL("open failed");

    /* Test chmod (via fchmodat) */
    TEST("chmod (fchmodat)");
    if (chmod(testfile, 0644) == 0) {
        struct stat st;
        stat(testfile, &st);
        if ((st.st_mode & 0777) == 0644) PASS();
        else FAIL("mode mismatch");
    } else FAIL("chmod failed");

    /* Test symlink */
    TEST("symlink");
    if (symlink(testfile, symlink_path) == 0) {
        struct stat st;
        if (lstat(symlink_path, &st) == 0 && S_ISLNK(st.st_mode)) {
            char buf[256];
            ssize_t len = readlink(symlink_path, buf, sizeof(buf) - 1);
            if (len > 0) {
                buf[len] = '\0';
                if (strcmp(buf, testfile) == 0) PASS();
                else FAIL("readlink mismatch");
            } else FAIL("readlink failed");
        } else FAIL("not a symlink");
    } else FAIL("symlink failed");

    /* Test hardlink */
    TEST("hardlink (link)");
    if (link(testfile, hardlink_path) == 0) {
        struct stat st1, st2;
        stat(testfile, &st1);
        stat(hardlink_path, &st2);
        if (st1.st_ino == st2.st_ino && st1.st_nlink >= 2) PASS();
        else FAIL("inode/nlink mismatch");
    } else FAIL("link failed");

    /* Test utimensat (set modification time) */
    TEST("utimensat");
    {
        struct timespec ts[2];
        ts[0].tv_sec = 1000000000;  /* atime */
        ts[0].tv_nsec = 0;
        ts[1].tv_sec = 1000000000;  /* mtime */
        ts[1].tv_nsec = 0;
        /* Use utimensat via open path */
        fd = open(testfile, O_RDONLY);
        if (fd >= 0) {
            /* futimens is utimensat with NULL path */
            struct timespec times[2] = {
                { .tv_sec = 1000000000, .tv_nsec = 0 },
                { .tv_sec = 1000000000, .tv_nsec = 0 }
            };
            /* Use utimensat(AT_FDCWD, path, times, 0) via the libc wrapper */
            if (utimensat(AT_FDCWD, testfile, times, 0) == 0) {
                struct stat st;
                stat(testfile, &st);
                if (st.st_mtime == 1000000000) PASS();
                else FAIL("mtime not updated");
            } else {
                /* If utimensat returns error, check errno */
                FAIL("utimensat failed");
            }
            close(fd);
        } else FAIL("open failed");
    }

    /* Test stat after operations */
    TEST("stat consistency");
    {
        struct stat st;
        if (stat(testfile, &st) == 0) {
            if (st.st_size == 6 && (st.st_mode & 0777) == 0644) PASS();
            else FAIL("stat mismatch");
        } else FAIL("stat failed");
    }

    /* Cleanup */
    unlink(hardlink_path);
    unlink(symlink_path);
    unlink(testfile);

    printf("\nResults: %d passed, %d failed\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
