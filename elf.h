/* elf.h — ELF64 parser and loader for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Parses static aarch64-linux ELF64 executables, extracts PT_LOAD segments,
 * and copies them into guest memory.
 */
#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

/* ---------- ELF64 structures (from Linux ABI) ---------- */

#define EI_NIDENT  16

/* ELF magic */
#define ELFMAG0    0x7f
#define ELFMAG1    'E'
#define ELFMAG2    'L'
#define ELFMAG3    'F'

/* e_ident indices */
#define EI_CLASS   4
#define EI_DATA    5

/* EI_CLASS values */
#define ELFCLASS64 2

/* EI_DATA values */
#define ELFDATA2LSB 1

/* e_type */
#define ET_EXEC    2
#define ET_DYN     3

/* e_machine */
#define EM_AARCH64 183

/* Program header types */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6
#define PT_GNU_STACK  0x6474e551
#define PT_GNU_RELRO  0x6474e552

/* Program header flags */
#define PF_X       1
#define PF_W       2
#define PF_R       4

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

/* ---------- Loaded ELF info ---------- */

#define ELF_MAX_SEGMENTS 16

typedef struct {
    /* From ELF header */
    uint64_t entry;        /* e_entry: program entry point */
    uint16_t e_type;       /* ET_EXEC or ET_DYN */
    uint16_t phnum;        /* Number of program headers */
    uint16_t phentsize;    /* Size of each program header */

    /* PT_LOAD segment bounds (for page table coverage) */
    uint64_t load_min;     /* Lowest loaded GPA (page-aligned) */
    uint64_t load_max;     /* Highest loaded GPA + memsz (page-aligned up) */

    /* Program headers location in guest memory (for AT_PHDR auxv) */
    uint64_t phdr_gpa;     /* GPA of program headers in guest memory */

    /* PT_INTERP: dynamic linker path (empty if statically linked) */
    char     interp_path[256];

    /* Segment details */
    int      num_segments;
    struct {
        uint64_t gpa;      /* Guest physical address */
        uint64_t offset;   /* File offset (p_offset, for /proc/self/maps) */
        uint64_t filesz;   /* Bytes to load from file */
        uint64_t memsz;    /* Total memory size (filesz + bss) */
        int      flags;    /* PF_R, PF_W, PF_X */
    } segments[ELF_MAX_SEGMENTS];
} elf_info_t;

/* ---------- API ---------- */

struct guest_t;  /* Forward declaration */

/* Load and parse an ELF64 file. Validates header, extracts PT_LOAD info.
 * Returns 0 on success, -1 on failure. Does NOT copy to guest yet. */
int elf_load(const char *path, elf_info_t *info);

/* Copy ELF segments into guest memory. Call after elf_load() and guest_init().
 * Also copies program headers into guest memory for AT_PHDR.
 * load_base is added to all virtual addresses (0 for ET_EXEC at link addr,
 * non-zero for ET_DYN loaded at a chosen base).
 * Returns 0 on success, -1 on failure. */
int elf_map_segments(const elf_info_t *info, const char *path,
                     void *guest_base, uint64_t guest_size,
                     uint64_t load_base);

#endif /* ELF_H */
