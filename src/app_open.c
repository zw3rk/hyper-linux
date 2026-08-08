/* app_open.c — Finder/Open-With path mapping without shell
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_open.h"
#include "vfs.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hl_app_host_path_to_guest(const char *host_path,
                              char *guest_out, size_t guest_sz) {
    if (!host_path || !guest_out || guest_sz == 0) return -1;
    if (hl_vfs_host_to_guest(host_path, guest_out, guest_sz) == 0) {
        if (hl_trace_on(HL_TRACE_FS))
            hl_trace(HL_TRACE_FS, "app open host=%s guest=%s",
                     host_path, guest_out);
        return 0;
    }
    return -1;
}

int hl_app_build_argv(const char *guest_binary,
                      char *const *host_paths, int npaths,
                      char ***out_argv) {
    if (!guest_binary || !out_argv || npaths < 0) return -1;
    char **argv = calloc((size_t)npaths + 2, sizeof(char *));
    if (!argv) return -1;
    argv[0] = strdup(guest_binary);
    if (!argv[0]) { free(argv); return -1; }
    int argc = 1;
    for (int i = 0; i < npaths; i++) {
        char guest[HL_VFS_PATH_MAX];
        if (hl_app_host_path_to_guest(host_paths[i], guest, sizeof(guest)) < 0) {
            /* Reject unmapped — do not silently rewrite */
            for (int j = 0; j < argc; j++) free(argv[j]);
            free(argv);
            return -1;
        }
        argv[argc] = strdup(guest);
        if (!argv[argc]) {
            for (int j = 0; j < argc; j++) free(argv[j]);
            free(argv);
            return -1;
        }
        argc++;
    }
    argv[argc] = NULL;
    *out_argv = argv;
    return argc;
}

void hl_app_argv_free(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}
