/* app_open.h — Host path → guest argv mapping for Finder opens (WS9)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * No shell: callers must execve/posix_spawn with argv arrays.
 */
#ifndef HL_APP_OPEN_H
#define HL_APP_OPEN_H

#include <stddef.h>

/* Map one absolute host path to a guest path using the VFS reverse map.
 * Returns 0 on success, -1 if unmapped (caller may reject or transient-bind). */
int hl_app_host_path_to_guest(const char *host_path,
                              char *guest_out, size_t guest_sz);

/* Build argv for launch: argv[0]=binary, then guest paths for each host path.
 * out_argv is newly allocated NULL-terminated; free with hl_app_argv_free.
 * Returns argc or -1. Never builds a shell command string. */
int hl_app_build_argv(const char *guest_binary,
                      char *const *host_paths, int npaths,
                      char ***out_argv);

void hl_app_argv_free(char **argv);

#endif /* HL_APP_OPEN_H */
