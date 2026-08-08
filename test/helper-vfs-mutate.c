/* helper-vfs-mutate.c — race a mutating op through a rename-swapped component.
 *
 * A flipper thread swaps "race" between a real dir and a symlink->.. while
 * the main thread loops unlink("race/secret.txt"). In rooted mode the guest
 * cannot see outside its bind, so the escape is verified HOST-side by
 * test-diagnostics.sh: the outside secret.txt must survive. String+realpath
 * checking loses this race; AT_RESOLVE_BENEATH / parent-pinning wins it.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>

static volatile int stop;
static void *flip(void *a) {
    (void)a;
    mkdir("d_real", 0755);
    symlink("..", "d_link");
    while (!stop) {
        rename("d_real", "race"); rename("race", "d_real");
        rename("d_link", "race"); rename("race", "d_link");
    }
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_t th;
    pthread_create(&th, NULL, flip, NULL);
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long tries = 0;
    for (;;) {
        unlink("race/secret.txt");      /* aim at the outside secret */
        int fd = open("race/secret.txt", O_WRONLY | O_TRUNC);
        if (fd >= 0) { write(fd, "PWNED", 5); close(fd); }
        if ((++tries & 0x3FFF) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= 3) break;
        }
    }
    stop = 1;
    pthread_join(th, NULL);
    printf("mutate-done\n");
    return 0;
}
