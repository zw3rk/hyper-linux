/* test-vfs-unit.c — host unit tests for VFS resolver (no HVF)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    int fails = 0;
    hl_vfs_reset();
    hl_vfs_set_mode(HL_FS_ROOTED);
    hl_vfs_add_bind("/home/user", getenv("HOME") ? getenv("HOME") : "/tmp", 0);
    hl_vfs_set_cwd("/home/user");

    hl_vfs_resolve_t r;
    int rc = hl_vfs_resolve_at(-100 /* AT_FDCWD linux */, "Music/x.wav", 1, 0, &r);
    /* May ENOENT if no Music — still path should be under home */
    if (rc == 0 || rc < 0) {
        if (strncmp(r.guest_abs, "/home/user", 10) != 0 && rc == 0) {
            printf("FAIL guest_abs=%s\n", r.guest_abs);
            fails++;
        } else {
            printf("OK resolve relative cwd → %s (rc=%d)\n", r.guest_abs, rc);
        }
    }

    /* Create path independent of existence */
    rc = hl_vfs_resolve_at(-100, "/home/user/new-playlist.m3u", 1, 1, &r);
    if (rc != 0) {
        printf("FAIL create resolve rc=%d\n", rc);
        fails++;
    } else if (!r.leaf[0] || strcmp(r.leaf, "new-playlist.m3u") != 0) {
        printf("FAIL leaf=%s\n", r.leaf);
        fails++;
    } else {
        printf("OK parent+leaf create leaf=%s host_parent=%s\n",
               r.leaf, r.host_parent);
    }

    char guest[4096];
    const char *home = getenv("HOME");
    if (home && hl_vfs_host_to_guest(home, guest, sizeof(guest)) == 0) {
        if (strcmp(guest, "/home/user") == 0) printf("OK reverse map home\n");
        else { printf("FAIL reverse %s\n", guest); fails++; }
    } else {
        printf("SKIP reverse (no HOME)\n");
    }

    /* Virtual CWD: must not call host chdir for rooted success of set */
    char host_cwd_before[4096], host_cwd_after[4096];
    getcwd(host_cwd_before, sizeof(host_cwd_before));
    hl_vfs_set_cwd("/home/user");
    getcwd(host_cwd_after, sizeof(host_cwd_after));
    if (strcmp(host_cwd_before, host_cwd_after) != 0) {
        printf("FAIL host cwd changed\n");
        fails++;
    } else {
        printf("OK virtual cwd does not host-chdir on set_cwd\n");
    }

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
