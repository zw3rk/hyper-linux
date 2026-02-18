/* elf.c — ELF64 parser and loader for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reads a static aarch64-linux ELF64 executable, validates the header,
 * extracts PT_LOAD segments, and copies them into guest memory.
 */
#include "elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int elf_load(const char *path, elf_info_t *info) {
    memset(info, 0, sizeof(*info));

    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }

    /* Read ELF header */
    elf64_ehdr_t ehdr;
    if (fread(&ehdr, sizeof(ehdr), 1, f) != 1) {
        fprintf(stderr, "%s: failed to read ELF header\n", path);
        fclose(f);
        return -1;
    }

    /* Validate magic */
    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        fprintf(stderr, "%s: not an ELF file\n", path);
        fclose(f);
        return -1;
    }

    /* Validate class (64-bit) */
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "%s: not a 64-bit ELF\n", path);
        fclose(f);
        return -1;
    }

    /* Validate endianness (little-endian for aarch64) */
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        fprintf(stderr, "%s: not little-endian\n", path);
        fclose(f);
        return -1;
    }

    /* Validate machine (aarch64) */
    if (ehdr.e_machine != EM_AARCH64) {
        fprintf(stderr, "%s: not aarch64 (e_machine=%u)\n", path, ehdr.e_machine);
        fclose(f);
        return -1;
    }

    /* Accept ET_EXEC (static) and ET_DYN (PIE, if loaded at fixed addr) */
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        fprintf(stderr, "%s: not an executable (e_type=%u)\n", path, ehdr.e_type);
        fclose(f);
        return -1;
    }

    info->entry = ehdr.e_entry;
    info->phnum = ehdr.e_phnum;
    info->phentsize = ehdr.e_phentsize;
    info->load_min = UINT64_MAX;
    info->load_max = 0;

    /* Read program headers */
    if (ehdr.e_phnum == 0) {
        fprintf(stderr, "%s: no program headers\n", path);
        fclose(f);
        return -1;
    }

    size_t ph_total = (size_t)ehdr.e_phnum * ehdr.e_phentsize;
    uint8_t *ph_buf = malloc(ph_total);
    if (!ph_buf) {
        perror("malloc");
        fclose(f);
        return -1;
    }

    if (fseek(f, ehdr.e_phoff, SEEK_SET) != 0 ||
        fread(ph_buf, ph_total, 1, f) != 1) {
        fprintf(stderr, "%s: failed to read program headers\n", path);
        free(ph_buf);
        fclose(f);
        return -1;
    }

    /* Parse PT_LOAD segments */
    int seg_count = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t *ph = (elf64_phdr_t *)(ph_buf + i * ehdr.e_phentsize);

        if (ph->p_type == PT_LOAD) {
            if (seg_count >= ELF_MAX_SEGMENTS) {
                fprintf(stderr, "%s: too many PT_LOAD segments\n", path);
                free(ph_buf);
                fclose(f);
                return -1;
            }

            info->segments[seg_count].gpa    = ph->p_vaddr;
            info->segments[seg_count].filesz = ph->p_filesz;
            info->segments[seg_count].memsz  = ph->p_memsz;
            info->segments[seg_count].flags  = ph->p_flags;
            seg_count++;

            /* Track load bounds */
            if (ph->p_vaddr < info->load_min)
                info->load_min = ph->p_vaddr;
            uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
            if (seg_end > info->load_max)
                info->load_max = seg_end;
        }
    }

    info->num_segments = seg_count;

    if (seg_count == 0) {
        fprintf(stderr, "%s: no PT_LOAD segments\n", path);
        free(ph_buf);
        fclose(f);
        return -1;
    }

    /* Store program header file offset for later phdr_gpa calculation.
     * We place program headers at the same GPA as they would be in
     * the first PT_LOAD segment (they're typically within it). */
    info->phdr_gpa = info->load_min + ehdr.e_phoff;

    free(ph_buf);
    fclose(f);
    return 0;
}

int elf_map_segments(const elf_info_t *info, const char *path,
                     void *guest_base, uint64_t guest_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }

    /* Re-read ELF header to get phoff */
    elf64_ehdr_t ehdr;
    if (fread(&ehdr, sizeof(ehdr), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    /* Read and parse program headers again to get file offsets */
    size_t ph_total = (size_t)ehdr.e_phnum * ehdr.e_phentsize;
    uint8_t *ph_buf = malloc(ph_total);
    if (!ph_buf) {
        fclose(f);
        return -1;
    }

    if (fseek(f, ehdr.e_phoff, SEEK_SET) != 0 ||
        fread(ph_buf, ph_total, 1, f) != 1) {
        free(ph_buf);
        fclose(f);
        return -1;
    }

    /* Copy program headers into guest memory at phdr_gpa
     * (needed for AT_PHDR auxv entry) */
    if (info->phdr_gpa + ph_total <= guest_size) {
        memcpy((uint8_t *)guest_base + info->phdr_gpa, ph_buf, ph_total);
    }

    /* Load each PT_LOAD segment */
    int seg_idx = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum && seg_idx < info->num_segments; i++) {
        elf64_phdr_t *ph = (elf64_phdr_t *)(ph_buf + i * ehdr.e_phentsize);

        if (ph->p_type != PT_LOAD)
            continue;

        uint64_t gpa = ph->p_vaddr;
        uint64_t filesz = ph->p_filesz;
        uint64_t memsz = ph->p_memsz;

        /* Bounds check */
        if (gpa + memsz > guest_size) {
            fprintf(stderr, "%s: segment at 0x%llx+0x%llx exceeds guest memory\n",
                    path, (unsigned long long)gpa, (unsigned long long)memsz);
            free(ph_buf);
            fclose(f);
            return -1;
        }

        /* Zero the entire memory region (covers BSS) */
        memset((uint8_t *)guest_base + gpa, 0, memsz);

        /* Copy file contents */
        if (filesz > 0) {
            if (fseek(f, ph->p_offset, SEEK_SET) != 0) {
                fprintf(stderr, "%s: seek failed for segment at 0x%llx\n",
                        path, (unsigned long long)gpa);
                free(ph_buf);
                fclose(f);
                return -1;
            }
            size_t nread = fread((uint8_t *)guest_base + gpa, 1, filesz, f);
            if (nread != filesz) {
                fprintf(stderr, "%s: short read for segment at 0x%llx "
                        "(got %zu, expected %llu)\n",
                        path, (unsigned long long)gpa, nread,
                        (unsigned long long)filesz);
                free(ph_buf);
                fclose(f);
                return -1;
            }
        }

        seg_idx++;
    }

    free(ph_buf);
    fclose(f);
    return 0;
}
