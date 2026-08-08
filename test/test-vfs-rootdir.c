/* test-vfs-rootdir.c — synthetic directories, outward symlinks, fchdir
 *
 * Three rooted-mode defects that all show up as "a perfectly ordinary
 * operation fails":
 *
 * (-) opendir("/") returned ENOENT. "/" and the interior components of a
 *     mount path (e.g. "/home" for a bind at /home/user) exist only in the
 *     mount table, so there is no host directory to open. FD_VIRTUAL_DIR
 *     was declared but never implemented, and every path fell through to
 *     ENOENT — `ls /` did not work at all.
 *
 * (-) The containment check canonicalized the FULL path to decide whether
 *     an operation escaped its bind. That resolves the final component
 *     too, so a symlink pointing out of the bind was refused with EACCES
 *     even for operations that never touch its target: readlink, lstat,
 *     unlink, rename. Creating such a link is legal (and common — dangling
 *     and outward symlinks appear in any real tree); only FOLLOWING it
 *     needs checking, and that path re-enters the resolver on the target.
 *
 * (-) fchdir() on a non-directory succeeded, because rooted mode only
 *     moved a virtual CWD string and never asked what the fd was. Linux
 *     returns ENOTDIR.
 *
 * (-) /proc/self/exe returned the HOST path of the binary. In rooted mode
 *     that leaks the host layout and username, and the guest cannot open
 *     it — anything re-execing itself through /proc/self/exe got ENOENT.
 *
 * (-) A bind whose host root did not exist yet was stored verbatim, while
 *     every other mount root is stored canonicalized. On macOS the two
 *     differ (/var/... vs /private/var/...), so the containment check
 *     compared a canonical parent against a non-canonical root, never
 *     matched, and the mount was dead with EACCES — permanently, even
 *     after the directory was created.
 *
 * Run with: hl --fs-mode=rooted --bind <tmp>:/home/user
 *              --bind <tmp>/late:/data --bind <testdir>:/opt/tests
 *              --guest-cwd /home/user
 * where <tmp>/late does NOT exist when hl starts, and <testdir> holds this
 * binary (so /proc/self/exe has a guest path to map back to).
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
#include <dirent.h>
#include <sys/stat.h>

static int dir_has(const char *path, const char *name) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, name) == 0) { found = 1; break; }
    closedir(d);
    return found;
}

int main(void) {
    int passes = 0, fails = 0;
    printf("test-vfs-rootdir: synthetic dirs, outward symlinks, fchdir\n");

    TEST("opendir(\"/\") lists the top-level mounts");
    {
        int r = dir_has("/", "home");
        if (r < 0) FAILF("opendir(/): %s", strerror(errno));
        else if (!r) FAIL("\"home\" missing from /");
        else PASS();
    }

    TEST("an interior mount component is a directory too");
    {
        int r = dir_has("/home", "user");
        if (r < 0) FAILF("opendir(/home): %s", strerror(errno));
        else if (!r) FAIL("\"user\" missing from /home");
        else PASS();
    }

    TEST("a synthetic directory contains . and ..");
    {
        int dot = dir_has("/", "."), dotdot = dir_has("/", "..");
        if (dot == 1 && dotdot == 1) PASS();
        else FAILF("dot=%d dotdot=%d", dot, dotdot);
    }

    TEST("stat and fstat agree that \"/\" is a directory");
    {
        struct stat s1, s2;
        int r1 = stat("/", &s1);
        int fd = open("/", O_RDONLY | O_DIRECTORY);
        int r2 = (fd >= 0) ? fstat(fd, &s2) : -1;
        if (r1 != 0) FAILF("stat(/): %s", strerror(errno));
        else if (fd < 0) FAILF("open(/): %s", strerror(errno));
        else if (r2 != 0) FAILF("fstat(/): %s", strerror(errno));
        else if (!S_ISDIR(s1.st_mode) || !S_ISDIR(s2.st_mode))
            FAILF("modes 0%o / 0%o", s1.st_mode, s2.st_mode);
        else PASS();
        if (fd >= 0) close(fd);
    }

    TEST("opening a synthetic directory for writing is EISDIR");
    {
        errno = 0;
        int fd = open("/", O_WRONLY);
        if (fd >= 0) { FAIL("opened / for writing"); close(fd); }
        else if (errno == EISDIR) PASS();
        else FAILF("errno %s, want EISDIR", strerror(errno));
    }

    /* Outward symlinks: legal to create, inspect and remove. */
    unlink("outward");
    TEST("a symlink pointing outside the bind can be created");
    if (symlink("/etc/hosts", "outward") == 0) PASS();
    else FAILF("symlink: %s", strerror(errno));

    TEST("readlink on an outward symlink returns the target");
    {
        char buf[128] = {0};
        ssize_t n = readlink("outward", buf, sizeof(buf) - 1);
        if (n < 0) FAILF("readlink: %s", strerror(errno));
        else if (strcmp(buf, "/etc/hosts") == 0) PASS();
        else FAILF("got \"%s\"", buf);
    }

    TEST("lstat on an outward symlink reports a symlink");
    {
        struct stat st;
        if (lstat("outward", &st) != 0) FAILF("lstat: %s", strerror(errno));
        else if (S_ISLNK(st.st_mode)) PASS();
        else FAILF("mode 0%o is not a symlink", st.st_mode);
    }

    TEST("renaming an outward symlink works");
    {
        if (rename("outward", "outward2") == 0) PASS();
        else FAILF("rename: %s", strerror(errno));
    }

    TEST("unlinking an outward symlink works");
    {
        if (unlink("outward2") == 0) PASS();
        else FAILF("unlink: %s", strerror(errno));
    }

    /* (+) Following one must still be contained: the target is re-resolved
     * through the mount table, so it must not reach the host's file. */
    TEST("following an outward symlink does not escape the bind");
    {
        unlink("escape");
        if (symlink("/etc/hosts", "escape") != 0) {
            FAILF("symlink: %s", strerror(errno));
        } else {
            int fd = open("escape", O_RDONLY);
            if (fd < 0) PASS();          /* target not mapped — contained */
            else { FAIL("read the host's /etc/hosts through the link");
                   close(fd); }
            unlink("escape");
        }
    }

    TEST("fchdir on a regular file is ENOTDIR");
    {
        int f = open("plainfile", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0) close(f);
        f = open("plainfile", O_RDONLY);
        if (f < 0) FAILF("open: %s", strerror(errno));
        else {
            errno = 0;
            int r = fchdir(f);
            if (r == 0) FAIL("succeeded — the CWD is now a regular file");
            else if (errno == ENOTDIR) PASS();
            else FAILF("errno %s, want ENOTDIR", strerror(errno));
            close(f);
        }
        unlink("plainfile");
    }

    TEST("fchdir on a real directory still works");
    {
        mkdir("adir", 0755);
        int d = open("adir", O_RDONLY | O_DIRECTORY);
        if (d < 0) FAILF("open: %s", strerror(errno));
        else {
            if (fchdir(d) == 0) PASS();
            else FAILF("fchdir: %s", strerror(errno));
            close(d);
        }
        if (chdir("..") != 0) { /* best effort */ }
        rmdir("adir");
    }

    TEST("/proc/self/exe is a guest path, and it opens");
    {
        char buf[512] = {0};
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) {
            FAILF("readlink: %s", strerror(errno));
        } else if (strncmp(buf, "/opt/tests/", 11) != 0) {
            FAILF("host path leaked: %s", buf);
        } else {
            int fd = open(buf, O_RDONLY);
            if (fd < 0) FAILF("open(%s): %s", buf, strerror(errno));
            else { close(fd); PASS(); }
        }
    }

    /* The /data mount's host root does not exist when hl starts; its parent
     * is this bind, so the guest can create it and the mount must then
     * work. Under the bug it stayed EACCES for the life of the process. */
    TEST("a bind whose root appears later is not dead");
    {
        mkdir("/home/user/late", 0755);
        int fd = open("/data/f", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            FAILF("open(/data/f): %s", strerror(errno));
        } else {
            ssize_t w = write(fd, "LATE", 4);
            close(fd);
            char buf[8] = {0};
            int rd = open("/data/f", O_RDONLY);
            ssize_t r = (rd >= 0) ? read(rd, buf, sizeof(buf) - 1) : -1;
            if (rd >= 0) close(rd);
            if (w != 4) FAILF("write: %zd", w);
            else if (r != 4 || strcmp(buf, "LATE") != 0)
                FAILF("read back %zd \"%s\"", r, buf);
            else PASS();
            unlink("/data/f");
        }
    }

    SUMMARY("test-vfs-rootdir");
    return fails ? 1 : 0;
}
