/* elf.c — ELF64 parser and loader for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reads aarch64-linux or x86_64-linux ELF64 executables, validates the
 * header, extracts PT_LOAD segments, and copies them into guest memory.
 * x86_64 binaries are executed via Apple's Rosetta Linux translator.
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

    /* Validate machine: aarch64 (native) or x86_64 (via rosetta) */
    if (ehdr.e_machine != EM_AARCH64 && ehdr.e_machine != EM_X86_64) {
        fprintf(stderr, "%s: unsupported architecture (e_machine=%u)\n",
                path, ehdr.e_machine);
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
    info->e_type = ehdr.e_type;
    info->e_machine = ehdr.e_machine;
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
    if (ehdr.e_phentsize < sizeof(elf64_phdr_t)) {
        fprintf(stderr, "%s: e_phentsize too small (%u < %zu)\n",
                path, ehdr.e_phentsize, sizeof(elf64_phdr_t));
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

    if (fseek(f, (long)ehdr.e_phoff, SEEK_SET) != 0 ||
        fread(ph_buf, ph_total, 1, f) != 1) {
        fprintf(stderr, "%s: failed to read program headers\n", path);
        free(ph_buf);
        fclose(f);
        return -1;
    }

    /* Parse program headers: PT_LOAD segments and PT_INTERP */
    int seg_count = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t *ph = (elf64_phdr_t *)(ph_buf + (size_t)i * ehdr.e_phentsize);

        /* PT_INTERP: read the dynamic linker path from the file */
        if (ph->p_type == PT_INTERP) {
            size_t interp_len = ph->p_filesz;
            if (interp_len >= sizeof(info->interp_path)) {
                fprintf(stderr, "%s: PT_INTERP path too long (%zu >= %zu)\n",
                        path, interp_len, sizeof(info->interp_path));
                free(ph_buf);
                fclose(f);
                return -1;
            }
            if (interp_len > 0) {
                long saved_pos = ftell(f);
                if (fseek(f, (long)ph->p_offset, SEEK_SET) == 0) {
                    size_t n = fread(info->interp_path, 1, interp_len, f);
                    /* interp_len includes the NUL from the ELF file.
                     * On short read, clear the path (unusable). On full
                     * read, force-terminate as insurance. */
                    if (n < interp_len)
                        info->interp_path[0] = '\0';
                    else
                        info->interp_path[interp_len - 1] = '\0';
                }
                fseek(f, saved_pos, SEEK_SET);
            }
        }

        if (ph->p_type == PT_LOAD) {
            if (seg_count >= ELF_MAX_SEGMENTS) {
                fprintf(stderr, "%s: too many PT_LOAD segments\n", path);
                free(ph_buf);
                fclose(f);
                return -1;
            }

            info->segments[seg_count].gpa    = ph->p_vaddr;
            info->segments[seg_count].offset = ph->p_offset;
            info->segments[seg_count].filesz = ph->p_filesz;
            info->segments[seg_count].memsz  = ph->p_memsz;
            info->segments[seg_count].flags  = (int)ph->p_flags;
            seg_count++;

            /* Track load bounds */
            if (ph->p_vaddr < info->load_min)
                info->load_min = ph->p_vaddr;
            uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
            if (seg_end < ph->p_vaddr) seg_end = UINT64_MAX; /* overflow */
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
                     void *guest_base, uint64_t guest_size,
                     uint64_t load_base) {
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

    if (fseek(f, (long)ehdr.e_phoff, SEEK_SET) != 0 ||
        fread(ph_buf, ph_total, 1, f) != 1) {
        free(ph_buf);
        fclose(f);
        return -1;
    }

    /* Copy program headers into guest memory at phdr_gpa + load_base
     * (needed for AT_PHDR auxv entry).  Fail hard if they don't fit —
     * a missing copy would leave AT_PHDR pointing at uninitialised
     * memory, crashing the dynamic linker. */
    /* Note: phdr_gpa + load_base may wrap via 2's complement for
     * high-VA binaries like rosetta (load_base is a negative offset
     * from 128TB VA to the low GPA). The bounds check below catches
     * invalid results. */
    uint64_t phdr_dest = info->phdr_gpa + load_base;
    if (phdr_dest + ph_total < phdr_dest ||
        phdr_dest + ph_total > guest_size) {
        fprintf(stderr, "%s: program headers at 0x%llx exceed guest memory "
                "(size 0x%llx)\n", path, (unsigned long long)(phdr_dest + ph_total),
                (unsigned long long)guest_size);
        free(ph_buf);
        fclose(f);
        return -1;
    }
    memcpy((uint8_t *)guest_base + phdr_dest, ph_buf, ph_total);

    /* Load each PT_LOAD segment (adjusted by load_base for ET_DYN) */
    int seg_idx = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum && seg_idx < info->num_segments; i++) {
        elf64_phdr_t *ph = (elf64_phdr_t *)(ph_buf + (size_t)i * ehdr.e_phentsize);

        if (ph->p_type != PT_LOAD)
            continue;

        /* Note: p_vaddr + load_base may wrap via 2's complement for
         * high-VA binaries (see comment above). Bounds check below
         * catches invalid results. */
        uint64_t gpa = ph->p_vaddr + load_base;
        uint64_t filesz = ph->p_filesz;
        uint64_t memsz = ph->p_memsz;

        /* Validate filesz <= memsz (ELF spec requirement) */
        if (filesz > memsz) {
            fprintf(stderr, "%s: segment at 0x%llx has filesz > memsz "
                    "(0x%llx > 0x%llx)\n",
                    path, (unsigned long long)gpa,
                    (unsigned long long)filesz, (unsigned long long)memsz);
            free(ph_buf);
            fclose(f);
            return -1;
        }

        /* Bounds check (overflow-safe) */
        if (memsz > guest_size || gpa > guest_size - memsz) {
            fprintf(stderr, "%s: segment at 0x%llx+0x%llx exceeds guest memory\n",
                    path, (unsigned long long)gpa, (unsigned long long)memsz);
            free(ph_buf);
            fclose(f);
            return -1;
        }

        /* Zero the entire memory region (covers BSS + page tail).
         * We zero up to the page-aligned end, not just memsz, because
         * programs like glibc's dynamic linker use a bump allocator
         * that extends past _end into the page tail. On real Linux,
         * the kernel maps zero-filled pages beyond memsz. If we only
         * zero up to memsz, stale data from a previous execve persists
         * in the page tail and corrupts the new program's allocator. */
        uint64_t zero_len = (memsz + 4095) & ~4095ULL;
        if (gpa + zero_len > guest_size)
            zero_len = guest_size - gpa;
        memset((uint8_t *)guest_base + gpa, 0, zero_len);

        /* Copy file contents */
        if (filesz > 0) {
            if (fseek(f, (long)ph->p_offset, SEEK_SET) != 0) {
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

void elf_resolve_interp(const char *sysroot, const char *interp_path,
                        char *out, size_t out_sz) {
    if (sysroot) {
        /* Strategy 1: sysroot + full interp path */
        snprintf(out, out_sz, "%s%s", sysroot, interp_path);
        if (access(out, F_OK) == 0)
            return;

        /* Strategy 2: sysroot/lib/basename — handles nix store paths
         * like /nix/store/...-musl-1.2.5/lib/ld-musl-aarch64.so.1 */
        const char *base = strrchr(interp_path, '/');
        base = base ? base + 1 : interp_path;
        snprintf(out, out_sz, "%s/lib/%s", sysroot, base);
        if (access(out, F_OK) == 0)
            return;
    }
    /* Strategy 3: use interp_path as-is */
    strncpy(out, interp_path, out_sz - 1);
    out[out_sz - 1] = '\0';
}
