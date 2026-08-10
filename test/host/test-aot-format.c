/* test-aot-format.c — Rosetta AOT stable-prefix regression tests
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "aot_format.h"

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

static int write_fixture(uint64_t total_size, uint64_t version,
                         uint64_t original_size, uint64_t code_offset,
                         off_t file_size) {
    char path[] = "/tmp/hl-aot-format.XXXXXX";
    uint8_t header[0x60] = {0};
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);

    memcpy(header + 0x00, &total_size, sizeof(total_size));
    memcpy(header + 0x08, &version, sizeof(version));
    memcpy(header + 0x10, &original_size, sizeof(original_size));
    memcpy(header + 0x18, &code_offset, sizeof(code_offset));

    /* Current macOS uses these bytes for different, non-stable fields. */
    header[0x50] = 0x00;
    header[0x51] = 0x40;
    header[0x52] = 0x03;
    header[0x54] = 0x0d;
    header[0x55] = 0x4f;
    header[0x56] = 0x04;

    if (pwrite(fd, header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
        ftruncate(fd, file_size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(void) {
    int fd = write_fixture(0x128000, 1, 0x1b2000, 0x1000, 0x2000);
    check(fd >= 0, "create current-format AOT fixture");
    if (fd >= 0) {
        check(hl_aot_header_valid(fd) == 0,
              "accept stable prefix when later fields changed");
        close(fd);
    }

    fd = write_fixture(0x128000, 2, 0x1b2000, 0x1000, 0x2000);
    check(fd >= 0 && hl_aot_header_valid(fd) == -1,
          "reject unknown AOT version");
    if (fd >= 0) close(fd);

    fd = write_fixture(0x128000, 1, 0x1b2000, 0x2000, 0x3000);
    check(fd >= 0 && hl_aot_header_valid(fd) == -1,
          "reject unexpected code offset");
    if (fd >= 0) close(fd);

    fd = write_fixture(0x128000, 1, 0x1b2000, 0x1000, 0x1000);
    check(fd >= 0 && hl_aot_header_valid(fd) == -1,
          "reject header without translated code");
    if (fd >= 0) close(fd);

    fd = write_fixture(0x128001, 1, 0x1b2000, 0x1000, 0x2000);
    check(fd >= 0 && hl_aot_header_valid(fd) == -1,
          "reject unaligned mapped size");
    if (fd >= 0) close(fd);

    fd = write_fixture(0x1000, 1, 0x1b2000, 0x1000, 0x2000);
    check(fd >= 0 && hl_aot_header_valid(fd) == -1,
          "reject mapped size without translated code");
    if (fd >= 0) close(fd);

    return failures ? 1 : 0;
}
