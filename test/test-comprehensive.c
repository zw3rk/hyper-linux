/* test-comprehensive.c — exercises all major hl subsystems
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: stdio, file I/O, dir listing, env vars, time, math, memory,
 * string ops, and system info — all in one program. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/utsname.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        failures++; \
    } \
} while(0)

int main(int argc, char *argv[]) {
    printf("=== Comprehensive Test ===\n\n");

    /* 1. argv */
    CHECK(argc >= 1, "argc >= 1");
    CHECK(argv[0] != NULL, "argv[0] not null");

    /* 2. Environment */
    CHECK(getenv("PATH") != NULL, "PATH set");

    /* 3. System info */
    struct utsname uts;
    CHECK(uname(&uts) == 0, "uname succeeds");
    CHECK(strcmp(uts.sysname, "Linux") == 0, "sysname == Linux");
    CHECK(strcmp(uts.machine, "aarch64") == 0, "machine == aarch64");

    /* 4. PID */
    CHECK(getpid() > 0, "getpid > 0");

    /* 5. Working directory */
    char cwd[256];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd succeeds");
    CHECK(strlen(cwd) > 0, "cwd not empty");

    /* 6. Malloc + realloc + calloc + free */
    int *arr = malloc(100 * sizeof(int));
    CHECK(arr != NULL, "malloc(400)");
    for (int i = 0; i < 100; i++) arr[i] = i;
    arr = realloc(arr, 200 * sizeof(int));
    CHECK(arr != NULL, "realloc(800)");
    CHECK(arr[50] == 50, "realloc preserves data");
    double *d = calloc(10, sizeof(double));
    CHECK(d != NULL, "calloc(80)");
    CHECK(d[5] == 0.0, "calloc zeroed");
    free(arr); free(d);

    /* 7. String operations */
    char buf[100];
    strcpy(buf, "hello");
    strcat(buf, " world");
    CHECK(strcmp(buf, "hello world") == 0, "strcpy+strcat+strcmp");
    CHECK(strlen(buf) == 11, "strlen");
    CHECK(strstr(buf, "world") != NULL, "strstr");

    /* 8. Math */
    CHECK(fabs(sqrt(4.0) - 2.0) < 1e-10, "sqrt(4)==2");
    CHECK(fabs(sin(0.0)) < 1e-10, "sin(0)==0");
    CHECK(fabs(cos(0.0) - 1.0) < 1e-10, "cos(0)==1");
    CHECK(fabs(log(M_E) - 1.0) < 1e-10, "log(e)==1");
    CHECK(fabs(pow(2.0, 10.0) - 1024.0) < 1e-10, "2^10==1024");

    /* 9. Time */
    time_t t = time(NULL);
    CHECK(t > 1700000000, "time > 2023");
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "clock_gettime");
    CHECK(ts.tv_sec >= 0, "monotonic >= 0");

    /* 10. File I/O roundtrip */
    const char *path = "/tmp/hl-comprehensive-test.txt";
    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL, "fopen(w)");
    fprintf(fp, "test data: %d\n", 42);
    fclose(fp);

    fp = fopen(path, "r");
    CHECK(fp != NULL, "fopen(r)");
    int val = 0;
    CHECK(fscanf(fp, "test data: %d", &val) == 1, "fscanf");
    CHECK(val == 42, "fscanf value == 42");
    fclose(fp);
    remove(path);

    /* 11. Directory listing */
    DIR *dir = opendir(".");
    CHECK(dir != NULL, "opendir(.)");
    int count = 0;
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir))) count++;
        closedir(dir);
    }
    CHECK(count > 0, "readdir found entries");

    /* 12. Printf formatting */
    char fmt[100];
    snprintf(fmt, sizeof(fmt), "%d %.2f %s %x", 42, 3.14, "hello", 255);
    CHECK(strcmp(fmt, "42 3.14 hello ff") == 0, "snprintf formatting");

    /* Summary */
    printf("\n=== Results: %d failures ===\n", failures);
    return failures > 0 ? 1 : 0;
}
