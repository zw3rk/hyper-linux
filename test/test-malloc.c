/* test-malloc.c — dynamic memory allocation test
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: malloc, realloc, calloc, free (brk and mmap syscalls). */

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* malloc + fill */
    int *arr = malloc(10 * sizeof(int));
    if (!arr) { printf("malloc failed!\n"); return 1; }
    for (int i = 0; i < 10; i++) arr[i] = i * i;
    printf("Squares: ");
    for (int i = 0; i < 10; i++) printf("%d ", arr[i]);
    printf("\n");

    /* realloc */
    int *bigger = realloc(arr, 20 * sizeof(int));
    if (!bigger) { printf("realloc failed!\n"); free(arr); return 1; }
    arr = bigger;
    for (int i = 10; i < 20; i++) arr[i] = i * i;
    printf("Extended: ");
    for (int i = 15; i < 20; i++) printf("%d ", arr[i]);
    printf("\n");

    /* calloc (zero-initialized) */
    double *d = calloc(5, sizeof(double));
    if (!d) { printf("calloc failed!\n"); free(arr); return 1; }
    printf("Calloc: ");
    for (int i = 0; i < 5; i++) printf("%.1f ", d[i]);
    printf("\n");

    free(arr);
    free(d);
    printf("OK\n");
    return 0;
}
