/* test-dev-stat.c — stat, fstat and statx must agree about a /dev node
 *
 * hl's virtual /dev nodes declare Linux device numbers (null is 1:3, zero
 * 1:5) but are backed by the corresponding HOST device. stat() and statx()
 * on the path went through the registry and reported the Linux numbers;
 * fstat() on an fd opened from the same path just fstat'd the host device
 * and reported the macOS ones — /dev/null was 1:3 by path and 3:2 by fd.
 *
 * Anything that identifies a device by its numbers (a "is this /dev/null"
 * check, a device-major dispatch) saw two different answers for one file.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* Linux struct statx, aarch64. Only the fields checked here matter. */
struct hl_statx {
    unsigned int  mask, blksize;
    unsigned long long attributes;
    unsigned int  nlink, uid, gid;
    unsigned short mode, pad1;
    unsigned long long ino, size, blocks, attributes_mask;
    long long atime_sec;  unsigned atime_nsec, pad2;
    long long btime_sec;  unsigned btime_nsec, pad3;
    long long ctime_sec;  unsigned ctime_nsec, pad4;
    long long mtime_sec;  unsigned mtime_nsec, pad5;
    unsigned int rdev_major, rdev_minor, dev_major, dev_minor;
    unsigned long long spare[14];
};

static void check(const char *path, unsigned want_maj, unsigned want_min,
                  int *passes, int *fails) {
    char label[64];
    snprintf(label, sizeof(label), "%s: stat/fstat/statx agree", path);

    struct stat sp;
    if (stat(path, &sp) != 0) {
        printf("  %-46s FAIL: stat: %s\n", label, strerror(errno));
        (*fails)++;
        return;
    }
    unsigned pmaj = major(sp.st_rdev), pmin = minor(sp.st_rdev);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("  %-46s FAIL: open: %s\n", label, strerror(errno));
        (*fails)++;
        return;
    }
    struct stat sf;
    int rf = fstat(fd, &sf);
    struct hl_statx sx;
    memset(&sx, 0, sizeof(sx));
    long rx = syscall(SYS_statx, AT_FDCWD, path, 0, 0x7ff, &sx);
    close(fd);

    if (rf != 0) {
        printf("  %-46s FAIL: fstat: %s\n", label, strerror(errno));
        (*fails)++;
    } else if (pmaj != want_maj || pmin != want_min) {
        printf("  %-46s FAIL: stat says %u:%u, want %u:%u\n",
               label, pmaj, pmin, want_maj, want_min);
        (*fails)++;
    } else if (major(sf.st_rdev) != want_maj || minor(sf.st_rdev) != want_min) {
        printf("  %-46s FAIL: fstat says %u:%u (host numbers), want %u:%u\n",
               label, major(sf.st_rdev), minor(sf.st_rdev), want_maj, want_min);
        (*fails)++;
    } else if (rx == 0 &&
               (sx.rdev_major != want_maj || sx.rdev_minor != want_min)) {
        printf("  %-46s FAIL: statx says %u:%u, want %u:%u\n",
               label, sx.rdev_major, sx.rdev_minor, want_maj, want_min);
        (*fails)++;
    } else if (!S_ISCHR(sf.st_mode)) {
        printf("  %-46s FAIL: fstat mode 0%o is not a char device\n",
               label, sf.st_mode);
        (*fails)++;
    } else {
        printf("  %-46s OK\n", label);
        (*passes)++;
    }
}

int main(void) {
    int passes = 0, fails = 0;
    printf("test-dev-stat: device numbers agree across stat/fstat/statx\n");

    /* Linux device numbers, from Documentation/admin-guide/devices.txt. */
    check("/dev/null",    1, 3, &passes, &fails);
    check("/dev/zero",    1, 5, &passes, &fails);
    check("/dev/urandom", 1, 9, &passes, &fails);

    /* (+) An ordinary file must be unaffected: rdev 0, not a char device. */
    TEST("a regular file still reports rdev 0");
    {
        int fd = open("devstat-plain", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        struct stat st;
        fd = open("devstat-plain", O_RDONLY);
        if (fd < 0) FAILF("open: %s", strerror(errno));
        else {
            int r = fstat(fd, &st);
            close(fd);
            if (r != 0) FAILF("fstat: %s", strerror(errno));
            else if (S_ISCHR(st.st_mode)) FAIL("reported as a char device");
            else if (st.st_rdev != 0) FAILF("rdev %llu, want 0",
                                            (unsigned long long)st.st_rdev);
            else PASS();
        }
        unlink("devstat-plain");
    }

    /* (V6) The registry numbers must survive dup / dup2 / F_DUPFD and fork —
     * the fix stored them on the fd; a dup/fork used to lose them and fall
     * back to the host device numbers. */
    TEST("dev numbers survive dup/dup2/F_DUPFD");
    {
        int fd = open("/dev/null", O_RDONLY);
        struct stat s0; fstat(fd, &s0);
        unsigned maj = major(s0.st_rdev), min = minor(s0.st_rdev);
        int d1 = dup(fd);
        int d2 = fcntl(fd, F_DUPFD, 50);
        int d3 = 60; d3 = dup2(fd, d3);
        struct stat a, b, c;
        int ok = fstat(d1, &a) == 0 && fstat(d2, &b) == 0 && fstat(d3, &c) == 0;
        if (!ok) FAILF("fstat dup: %s", strerror(errno));
        else if (major(a.st_rdev) != maj || minor(a.st_rdev) != min)
            FAILF("dup rdev %u:%u, want %u:%u", major(a.st_rdev), minor(a.st_rdev), maj, min);
        else if (major(b.st_rdev) != maj || minor(b.st_rdev) != min)
            FAILF("F_DUPFD rdev %u:%u, want %u:%u", major(b.st_rdev), minor(b.st_rdev), maj, min);
        else if (major(c.st_rdev) != maj || minor(c.st_rdev) != min)
            FAILF("dup2 rdev %u:%u, want %u:%u", major(c.st_rdev), minor(c.st_rdev), maj, min);
        else PASS();
        close(fd); close(d1); close(d2); close(d3);
    }

    TEST("dev numbers survive fork");
    {
        int fd = open("/dev/null", O_RDONLY);
        struct stat s0; fstat(fd, &s0);
        unsigned maj = major(s0.st_rdev), min = minor(s0.st_rdev);
        pid_t pid = fork();
        if (pid == 0) {
            struct stat cs;
            int r = fstat(fd, &cs);
            _exit((r == 0 && major(cs.st_rdev) == maj && minor(cs.st_rdev) == min)
                  ? 0 : 3);
        }
        int st = 0; waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) PASS();
        else FAILF("child fstat lost the dev numbers (status 0x%x)", st);
        close(fd);
    }

    /* (V15) AT_EMPTY_PATH: fstatat and statx on the fd itself must report
     * the registry numbers, not the host device (fstatat) or a socket
     * (statx on an OSS fd). */
    TEST("fstatat(AT_EMPTY_PATH) on /dev/null reports 1:3");
    {
        int fd = open("/dev/null", O_RDONLY);
        struct stat st;
        long r = syscall(SYS_newfstatat, fd, "", &st, 0x1000 /*AT_EMPTY_PATH*/);
        if (r != 0) FAILF("fstatat AT_EMPTY: %s", strerror(errno));
        else if (major(st.st_rdev) != 1 || minor(st.st_rdev) != 3)
            FAILF("rdev %u:%u, want 1:3", major(st.st_rdev), minor(st.st_rdev));
        else if (!S_ISCHR(st.st_mode)) FAIL("not a char device");
        else PASS();
        close(fd);
    }

    TEST("statx(AT_EMPTY_PATH) on /dev/null reports 1:3");
    {
        int fd = open("/dev/null", O_RDONLY);
        struct hl_statx sx; memset(&sx, 0, sizeof(sx));
        long r = syscall(SYS_statx, fd, "", 0x1000 /*AT_EMPTY_PATH*/, 0x7ff, &sx);
        if (r != 0) FAILF("statx AT_EMPTY: %s", strerror(errno));
        else if (sx.rdev_major != 1 || sx.rdev_minor != 3)
            FAILF("statx rdev %u:%u, want 1:3", sx.rdev_major, sx.rdev_minor);
        else PASS();
        close(fd);
    }

    TEST("statx(AT_EMPTY_PATH) on /dev/mixer is a char device 14:0");
    {
        int fd = open("/dev/mixer", O_RDONLY);
        if (fd < 0) { printf("(skip: no /dev/mixer) "); PASS(); }
        else {
            struct hl_statx sx; memset(&sx, 0, sizeof(sx));
            long r = syscall(SYS_statx, fd, "", 0x1000, 0x7ff, &sx);
            if (r != 0) FAILF("statx AT_EMPTY: %s", strerror(errno));
            else if (!((sx.mode & 0170000) == 0020000))   /* S_IFCHR */
                FAILF("mode 0%o is not a char device (S_IFSOCK?)", sx.mode);
            else if (sx.rdev_major != 14)
                FAILF("rdev major %u, want 14", sx.rdev_major);
            else PASS();
            close(fd);
        }
    }

    SUMMARY("test-dev-stat");
    return fails ? 1 : 0;
}
