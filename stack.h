/* stack.h — Linux initial stack builder for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Constructs the initial stack layout expected by the Linux ABI:
 * argc, argv pointers, envp pointers, auxiliary vector, and string data.
 */
#ifndef STACK_H
#define STACK_H

#include <stdint.h>
#include "guest.h"
#include "elf.h"

/* ---------- Auxiliary vector types ---------- */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_HWCAP   16
#define AT_CLKTCK  17
#define AT_RANDOM  25
#define AT_EXECFN  31
#define AT_PLATFORM 45

/* Build a Linux-compatible initial stack at the given stack_top.
 * Passes argc/argv and envp (NULL-terminated array of "KEY=val" strings).
 * elf_load_base is the base address PIE executables were loaded at (0 for ET_EXEC).
 * interp_base is the load base of the dynamic linker (0 if statically linked).
 * Returns the initial SP (stack pointer) to pass to the guest. */
uint64_t build_linux_stack(guest_t *g, uint64_t stack_top,
                           int argc, const char **argv,
                           const char **envp,
                           const elf_info_t *elf_info,
                           uint64_t elf_load_base,
                           uint64_t interp_base);

#endif /* STACK_H */
