/* test-fd-digest.c — position-independent Rosetta binary digest regression
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fd_digest.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *message) {
    if (condition) {
        printf("PASS: %s\n", message);
    } else {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    static const char payload[] = "hyper-linux\n";
    static const unsigned char expected[HL_SHA256_DIGEST_SIZE] = {
        0x0f, 0xab, 0x7b, 0x9f, 0xba, 0x15, 0xec, 0x46,
        0xbd, 0x6b, 0x28, 0xb2, 0x7a, 0x19, 0xc8, 0xb3,
        0x8c, 0x0c, 0x29, 0xdb, 0x13, 0x07, 0xc5, 0xbf,
        0x2c, 0xed, 0x76, 0x24, 0x8e, 0x28, 0xd0, 0x79,
    };
    char path[] = "/tmp/hl-fd-digest.XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "create digest fixture");
    if (fd < 0) return 1;
    unlink(path);

    check(write(fd, payload, sizeof(payload) - 1) ==
              (ssize_t)(sizeof(payload) - 1),
          "write digest fixture");
    check(lseek(fd, 4, SEEK_SET) == 4, "set caller-owned file offset");

    unsigned char digest[HL_SHA256_DIGEST_SIZE];
    check(hl_fd_sha256(fd, digest) == 0, "hash regular file descriptor");
    check(memcmp(digest, expected, sizeof(expected)) == 0,
          "digest covers the complete file");
    check(lseek(fd, 0, SEEK_CUR) == 4,
          "digest preserves the shared open-file offset");
    close(fd);

    int pipe_fds[2];
    int pipe_status = pipe(pipe_fds);
    check(pipe_status == 0, "create non-regular descriptor fixture");
    if (pipe_status == 0) {
        errno = 0;
        check(hl_fd_sha256(pipe_fds[0], digest) == -1,
              "digest rejects non-regular descriptors");
        check(errno == EINVAL,
              "non-regular digest failure reports a stable error");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
    }

    return failures ? 1 : 0;
}
