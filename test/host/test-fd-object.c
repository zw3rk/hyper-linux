/* test-fd-object.c — open-file lifetime, install, dup, close (host, no HVF)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drives real hl_open_file_create / hl_fd_install / hl_fd_get / hl_fd_dup
 * / hl_fd_remove against the shipped fd_object implementation.
 */
#include "fd_object.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

/* Minimal init: clear table and seed stdio like syscall_init does. */
extern void syscall_init(void);

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL %s\n", msg); fails++; } \
    else printf("OK %s\n", msg); \
} while (0)

static void of_destroy(hl_open_file_t *of) {
    free(of->state);
    of->state = NULL;
}

static const hl_fd_ops_t test_ops = {
    .destroy = of_destroy,
};

int main(void) {
    /* Initialize FD subsystem without full HVF */
    memset(fd_table, 0, sizeof(fd_entry_t) * FD_TABLE_SIZE);
    /* Need bitmap free — use fd_alloc path after manual init */
    extern void syscall_init(void);
    /* Avoid full signal/timer init if possible — call syscall_init carefully.
     * It needs eventfd etc. Prefer light init: */
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        fd_table[i].type = FD_CLOSED;
        fd_table[i].host_fd = -1;
        fd_table[i].of = NULL;
        fd_table[i].desc = NULL;
    }
    /* Use real syscall_init for bitmap correctness */
    syscall_init();

    /* Create open-file + install */
    int *st = malloc(sizeof(int));
    *st = 42;
    hl_open_file_t *of = hl_open_file_create(FD_REGULAR, &test_ops, 0, st);
    CHECK(of != NULL, "open_file_create");
    CHECK(atomic_load(&of->refcount) == 1, "refcount 1");

    int pipefd[2];
    CHECK(pipe(pipefd) == 0, "pipe");
    int gfd = hl_fd_install(of, pipefd[0], 0);
    CHECK(gfd >= 0, "install");
    CHECK(fd_table[gfd].type == FD_REGULAR, "table type");
    CHECK(fd_table[gfd].of == of, "table of");
    CHECK(fd_table[gfd].desc != NULL, "table desc");

    /* get/put pins open-file */
    hl_fd_ref_t ref;
    CHECK(hl_fd_get(gfd, &ref) == 0, "fd_get");
    CHECK(ref.of == of, "ref.of");
    CHECK(ref.host_fd == pipefd[0], "ref.host_fd");
    hl_fd_put(&ref);

    /* dup shares open-file */
    int gfd2 = hl_fd_dup(gfd);
    CHECK(gfd2 >= 0 && gfd2 != gfd, "dup");
    CHECK(fd_table[gfd2].of == of, "dup shares of");
    CHECK(atomic_load(&of->refcount) >= 2, "refcount after dup");

    /* close one side — open-file survives */
    hl_fd_detached_t det;
    CHECK(hl_fd_remove(gfd, &det) == 0, "remove first");
    hl_fd_detached_finish(&det);
    CHECK(fd_table[gfd].type == FD_CLOSED, "slot closed");
    CHECK(fd_table[gfd2].of != NULL, "sibling still open");

    /* final close destroys */
    CHECK(hl_fd_remove(gfd2, &det) == 0, "remove second");
    hl_fd_detached_finish(&det);
    CHECK(fd_table[gfd2].type == FD_CLOSED, "second closed");

    close(pipefd[1]);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
