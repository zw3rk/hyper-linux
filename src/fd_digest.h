/* fd_digest.h — position-independent SHA-256 for shared regular-file fds
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef HL_FD_DIGEST_H
#define HL_FD_DIGEST_H

#include <CommonCrypto/CommonDigest.h>

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define HL_SHA256_DIGEST_SIZE CC_SHA256_DIGEST_LENGTH

/* Hash a regular file without changing its shared open-file offset. Rosetta
 * retains and may use the same open file description after passing it through
 * SCM_RIGHTS, so lseek()+read() is not safe here. */
static inline int
hl_fd_sha256(int fd, uint8_t digest[HL_SHA256_DIGEST_SIZE])
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);

    uint8_t buffer[65536];
    off_t offset = 0;
    for (;;) {
        ssize_t count = pread(fd, buffer, sizeof(buffer), offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (count == 0) break;
        CC_SHA256_Update(&context, buffer, (CC_LONG)count);
        offset += count;
    }

    CC_SHA256_Final(digest, &context);
    return 0;
}

#endif /* HL_FD_DIGEST_H */
