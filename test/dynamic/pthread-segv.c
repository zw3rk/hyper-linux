/* pthread-segv.c — fatal worker signal must terminate the thread group
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <pthread.h>
#include <stdint.h>

static void *crash_worker(void *argument) {
    void (*volatile crash)(void) = (void (*)(void))(uintptr_t)argument;
    crash();
    return NULL;
}

int main(void) {
    pthread_t thread;

    if (pthread_create(&thread, NULL, crash_worker, NULL) != 0)
        return 1;
    if (pthread_join(thread, NULL) != 0)
        return 2;
    return 3;
}
