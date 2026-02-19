/* test-socket.c — Test socket syscalls (AF_UNIX socketpair)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies socket syscalls work by creating an AF_UNIX socketpair
 * and exchanging data. This avoids needing a network stack.
 *
 * Tests:
 * 1. socketpair(AF_UNIX, SOCK_STREAM) creates two connected fds
 * 2. write/read through socketpair transfers data correctly
 * 3. getsockopt(SO_TYPE) returns SOCK_STREAM
 * 4. shutdown(SHUT_WR) causes read to return 0 (EOF)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

int main(void) {
    int failures = 0;
    int sv[2];

    /* Test 1: socketpair */
    printf("test-socket: 1. socketpair(AF_UNIX)... ");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        printf("FAIL (socketpair: %m)\n");
        return 1;
    }
    if (sv[0] >= 0 && sv[1] >= 0) {
        printf("PASS (fds=%d,%d)\n", sv[0], sv[1]);
    } else {
        printf("FAIL (bad fds)\n");
        failures++;
    }

    /* Test 2: send/recv data */
    printf("test-socket: 2. write/read through socketpair... ");
    const char *msg = "hello socket";
    ssize_t n = write(sv[0], msg, strlen(msg));
    if (n != (ssize_t)strlen(msg)) {
        printf("FAIL (write returned %zd)\n", n);
        failures++;
    } else {
        char buf[64] = {0};
        n = read(sv[1], buf, sizeof(buf) - 1);
        if (n == (ssize_t)strlen(msg) && memcmp(buf, msg, strlen(msg)) == 0) {
            printf("PASS\n");
        } else {
            printf("FAIL (read returned %zd, got '%s')\n", n, buf);
            failures++;
        }
    }

    /* Test 3: getsockopt SO_TYPE */
    printf("test-socket: 3. getsockopt(SO_TYPE)... ");
    int sock_type = 0;
    socklen_t optlen = sizeof(sock_type);
    if (getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &sock_type, &optlen) < 0) {
        printf("FAIL (getsockopt: %m)\n");
        failures++;
    } else if (sock_type == SOCK_STREAM) {
        printf("PASS\n");
    } else {
        printf("FAIL (type=%d, expected %d)\n", sock_type, SOCK_STREAM);
        failures++;
    }

    /* Test 4: shutdown + EOF */
    printf("test-socket: 4. shutdown(SHUT_WR) → EOF... ");
    shutdown(sv[0], SHUT_WR);
    char buf2[16];
    n = read(sv[1], buf2, sizeof(buf2));
    if (n == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL (read after shutdown returned %zd)\n", n);
        failures++;
    }

    close(sv[0]);
    close(sv[1]);

    if (failures == 0) {
        printf("test-socket: all tests passed — PASS\n");
        return 0;
    }
    printf("test-socket: %d test(s) failed — FAIL\n", failures);
    return 1;
}
