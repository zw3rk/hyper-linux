/* test-app-open.c — reverse map argv without shell (WS9)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_open.h"
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int fails = 0;
    hl_vfs_reset();
    hl_vfs_set_mode(HL_FS_ROOTED);
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    hl_vfs_add_bind("/home/user", home, 0);

    char host_file[4096];
    snprintf(host_file, sizeof(host_file), "%s/Music/track with spaces.wav", home);

    char guest[4096];
    if (hl_app_host_path_to_guest(host_file, guest, sizeof(guest)) != 0) {
        printf("FAIL map\n");
        fails++;
    } else if (strstr(guest, "track with spaces.wav") == NULL) {
        printf("FAIL guest=%s\n", guest);
        fails++;
    } else {
        printf("OK map → %s\n", guest);
    }

    char *hosts[] = { host_file };
    char **argv = NULL;
    int argc = hl_app_build_argv("/usr/bin/xmms", hosts, 1, &argv);
    if (argc != 2 || !argv || !argv[0] || !argv[1]) {
        printf("FAIL argv build argc=%d\n", argc);
        fails++;
    } else if (strchr(argv[1], ' ') == NULL) {
        printf("FAIL spaces not preserved: %s\n", argv[1]);
        fails++;
    } else {
        printf("OK argv[0]=%s argv[1]=%s (no shell)\n", argv[0], argv[1]);
    }
    hl_app_argv_free(argv);

    /* Unmapped path must fail clearly */
    char *bad[] = { "/no/such/bind/path.wav" };
    argc = hl_app_build_argv("/usr/bin/xmms", bad, 1, &argv);
    if (argc != -1) {
        printf("FAIL unmapped should reject\n");
        fails++;
        hl_app_argv_free(argv);
    } else {
        printf("OK unmapped rejected\n");
    }

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
