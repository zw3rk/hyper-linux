/* test-roundtrip.c — file write+read roundtrip test
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: fopen(w), fwrite, fclose, fopen(r), fread, fclose, remove.
 * Verifies data integrity through a complete write→read cycle. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *path = "/tmp/hl-roundtrip-test.bin";
    const int N = 1000;

    /* Generate test data: array of squares */
    int *data = malloc(N * sizeof(int));
    if (!data) { perror("malloc"); return 1; }
    for (int i = 0; i < N; i++) data[i] = i * i;

    /* Write */
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("fopen(w)"); return 1; }
    size_t nw = fwrite(data, sizeof(int), N, fp);
    fclose(fp);
    printf("Wrote %zu ints\n", nw);

    /* Read back */
    int *buf = calloc(N, sizeof(int));
    if (!buf) { perror("calloc"); return 1; }
    fp = fopen(path, "rb");
    if (!fp) { perror("fopen(r)"); return 1; }
    size_t nr = fread(buf, sizeof(int), N, fp);
    fclose(fp);
    printf("Read %zu ints\n", nr);

    /* Verify */
    int ok = 1;
    for (int i = 0; i < N; i++) {
        if (buf[i] != i * i) {
            printf("MISMATCH at [%d]: expected %d, got %d\n", i, i*i, buf[i]);
            ok = 0;
            break;
        }
    }

    if (ok) printf("Data verified OK\n");

    free(data);
    free(buf);
    remove(path);

    return ok ? 0 : 1;
}
