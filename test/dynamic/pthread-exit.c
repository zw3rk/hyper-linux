/* pthread-exit.c — nested pthread join followed by immediate exit_group
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

static void *inner(void *argument) {
    return argument;
}

static void *outer(void *argument) {
    pthread_t thread;
    void *result = NULL;

    if (pthread_create(&thread, NULL, inner, argument) != 0)
        return (void *)(intptr_t)1;
    if (pthread_join(thread, &result) != 0 || result != argument)
        return (void *)(intptr_t)2;
    pthread_exit(NULL);
    return (void *)(intptr_t)3;
}

int main(void) {
    pthread_t thread;
    void *result = NULL;
    void *token = (void *)(intptr_t)42;

    if (pthread_create(&thread, NULL, outer, token) != 0)
        return 1;
    if (pthread_join(thread, &result) != 0 || result != NULL)
        return 2;

    _exit(0);
    return 3;
}
