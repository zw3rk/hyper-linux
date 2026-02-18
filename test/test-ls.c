/* test-ls.c — directory listing test
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: openat (O_DIRECTORY), getdents64, close. */

#include <stdio.h>
#include <dirent.h>

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : ".";

    DIR *d = opendir(path);
    if (!d) {
        perror(path);
        return 1;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        char type = '?';
        switch (ent->d_type) {
        case DT_REG: type = '-'; break;
        case DT_DIR: type = 'd'; break;
        case DT_LNK: type = 'l'; break;
        }
        printf("%c %s\n", type, ent->d_name);
        count++;
    }
    closedir(d);
    printf("Total: %d entries\n", count);
    return 0;
}
