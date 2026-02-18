/* stack.c — Linux initial stack builder for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Constructs the initial stack layout that the Linux ABI requires at
 * process startup. The stack is built at the top of the guest stack
 * region and grows downward: string data at the top, then the structured
 * area (auxv, envp, argv, argc) below.
 */
#include "stack.h"

#include <string.h>
#include <unistd.h>
#include <sys/random.h>

/* Push a uint64_t onto the stack (growing downward) */
static void push_u64(guest_t *g, uint64_t *sp, uint64_t val) {
    *sp -= 8;
    guest_write(g, *sp, &val, 8);
}

/* Write a string to guest memory at the given address, return length+1 */
static size_t write_str(guest_t *g, uint64_t gva, const char *s) {
    size_t len = strlen(s) + 1;
    guest_write(g, gva, s, len);
    return len;
}

uint64_t build_linux_stack(guest_t *g, uint64_t stack_top,
                           int argc, const char **argv,
                           const char **envp,
                           const elf_info_t *elf_info) {
    /*
     * Linux initial stack layout (growing from high to low):
     *   [ 16 random bytes for AT_RANDOM ]
     *   [ "aarch64\0" for AT_PLATFORM ]
     *   [ environment strings ]
     *   [ argument strings ]
     *   [ padding to 16-byte alignment ]
     *   [ AT_NULL (0, 0) ]
     *   [ auxv entries (key, value) pairs ]
     *   [ NULL (end of envp) ]
     *   [ envp[0], envp[1], ... ]
     *   [ NULL (end of argv) ]
     *   [ argv[argc-1] ... argv[0] ]
     *   [ argc ]                    <-- SP points here
     */

    /* Count environment entries */
    int envc = 0;
    if (envp) {
        while (envp[envc]) envc++;
    }

    /* Phase 1: Write strings and random data at the top of the stack.
     * We work downward from stack_top. */
    uint64_t str_ptr = stack_top;

    /* AT_RANDOM: 16 random bytes */
    str_ptr -= 16;
    uint64_t random_ptr = str_ptr;
    uint8_t random_bytes[16];
    getentropy(random_bytes, 16);
    guest_write(g, random_ptr, random_bytes, 16);

    /* AT_PLATFORM: "aarch64\0" */
    str_ptr -= 8;  /* strlen("aarch64") + 1 */
    uint64_t platform_ptr = str_ptr;
    write_str(g, platform_ptr, "aarch64");

    /* Environment strings */
    uint64_t env_ptrs[4096];
    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        str_ptr -= len;
        env_ptrs[i] = str_ptr;
        write_str(g, str_ptr, envp[i]);
    }

    /* Argument strings (written backward so argv[0] is at lowest addr) */
    uint64_t arg_ptrs[256];
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        str_ptr -= len;
        arg_ptrs[i] = str_ptr;
        write_str(g, str_ptr, argv[i]);
    }

    /* Phase 2: Build the structured part of the stack.
     * Align str_ptr down to 16 bytes first. */
    str_ptr &= ~15ULL;
    uint64_t sp = str_ptr;

    /* The structured area (auxv + envp + argv + argc) is pushed below
     * the string area. SP must be 16-byte aligned AND point to argc.
     * We cannot align after pushing (that creates a gap above argc).
     * Instead, compute total entries and add padding here if needed.
     *
     * 14 auxv pairs × 2 = 28, AT_NULL = 2, envp_null = 1,
     * envp_ptrs = envc, argv_null = 1, argv_ptrs = argc, argc_val = 1.
     * Total entries = 28 + 2 + 1 + envc + 1 + argc + 1 = 33 + argc + envc.
     * For 16-byte alignment: total must be even. */
    if ((33 + argc + envc) & 1) {
        push_u64(g, &sp, 0);  /* alignment padding */
    }

    /* Auxv entries (pushed in reverse order since stack grows down).
     * We push AT_NULL last (first in stack order). */
    push_u64(g, &sp, 0); push_u64(g, &sp, AT_NULL);

    push_u64(g, &sp, platform_ptr); push_u64(g, &sp, AT_PLATFORM);
    push_u64(g, &sp, random_ptr); push_u64(g, &sp, AT_RANDOM);
    push_u64(g, &sp, 100); push_u64(g, &sp, AT_CLKTCK);

    /* HWCAP: advertise basic aarch64 features
     * Bit 0=FP, Bit 1=ASIMD, Bit 3=AES, Bit 4=PMULL, etc. */
    push_u64(g, &sp, 0xFF); push_u64(g, &sp, AT_HWCAP);

    push_u64(g, &sp, 1000); push_u64(g, &sp, AT_EGID);
    push_u64(g, &sp, 1000); push_u64(g, &sp, AT_GID);
    push_u64(g, &sp, 1000); push_u64(g, &sp, AT_EUID);
    push_u64(g, &sp, 1000); push_u64(g, &sp, AT_UID);

    push_u64(g, &sp, elf_info->entry); push_u64(g, &sp, AT_ENTRY);
    push_u64(g, &sp, elf_info->phnum); push_u64(g, &sp, AT_PHNUM);
    push_u64(g, &sp, elf_info->phentsize); push_u64(g, &sp, AT_PHENT);
    push_u64(g, &sp, elf_info->phdr_gpa); push_u64(g, &sp, AT_PHDR);
    push_u64(g, &sp, 4096); push_u64(g, &sp, AT_PAGESZ);

    /* envp: environment variable pointers + NULL terminator */
    push_u64(g, &sp, 0);  /* NULL terminator */
    for (int i = envc - 1; i >= 0; i--) {
        push_u64(g, &sp, env_ptrs[i]);
    }

    /* argv: NULL terminator, then pointers in reverse order */
    push_u64(g, &sp, 0);  /* NULL terminator */
    for (int i = argc - 1; i >= 0; i--) {
        push_u64(g, &sp, arg_ptrs[i]);
    }

    /* argc — SP now points here, 16-byte aligned */
    push_u64(g, &sp, (uint64_t)argc);

    return sp;
}
