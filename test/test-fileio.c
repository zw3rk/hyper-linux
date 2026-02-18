/* test-fileio.c — file I/O test
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: openat, read, close, fstat (via stdio fopen/fgets/fclose). */

#include <stdio.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        perror(argv[1]);
        return 1;
    }

    char buf[256];
    int lineno = 0;
    while (fgets(buf, sizeof(buf), fp) && lineno < 3) {
        lineno++;
        printf("line %d: %s", lineno, buf);
    }

    /* Count remaining lines */
    while (fgets(buf, sizeof(buf), fp))
        lineno++;

    fclose(fp);
    printf("Total: %d lines\n", lineno);
    return 0;
}
