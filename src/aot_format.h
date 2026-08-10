/* aot_format.h — stable-prefix validation for Rosetta Linux AOT files
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef HL_AOT_FORMAT_H
#define HL_AOT_FORMAT_H

#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define HL_AOT_CODE_OFFSET 0x1000ULL

/* Rosetta's fields after offset 0x20 have changed between macOS releases.
 * Validate only the common prefix plus the presence of translated code.
 * Content checksums protect cached files from later corruption. */
static inline int
hl_aot_header_valid(int fd)
{
    struct stat st;
    uint8_t header[0x20];
    uint64_t total_size;
    uint64_t version;
    uint64_t original_size;
    uint64_t code_offset;

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= (off_t)HL_AOT_CODE_OFFSET)
        return -1;
    if (pread(fd, header, sizeof(header), 0) != (ssize_t)sizeof(header))
        return -1;

    memcpy(&total_size, header + 0x00, sizeof(total_size));
    memcpy(&version, header + 0x08, sizeof(version));
    memcpy(&original_size, header + 0x10, sizeof(original_size));
    memcpy(&code_offset, header + 0x18, sizeof(code_offset));

    if (version != 1 || code_offset != HL_AOT_CODE_OFFSET ||
        total_size <= HL_AOT_CODE_OFFSET || original_size == 0 ||
        (total_size & (HL_AOT_CODE_OFFSET - 1)) != 0 ||
        (original_size & (HL_AOT_CODE_OFFSET - 1)) != 0)
        return -1;
    return 0;
}

#endif /* HL_AOT_FORMAT_H */
