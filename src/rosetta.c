/* rosetta.c — Rosetta x86_64-linux translator setup for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Factored out of hl.c so that both main() and sys_execve() can set up
 * rosetta without duplicating ~250 lines of placement/mapping/argv logic.
 *
 * Two-phase API:
 *   rosetta_prepare()  — before guest_build_page_tables()
 *   rosetta_finalize() — after  guest_build_page_tables()
 */
#include "rosetta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>

#include "guest.h"
#include "elf.h"
#include "vdso.h"
#include "stack.h"
#include "syscall.h"
#include "syscall_internal.h"   /* fd_alloc_at, LINUX_PATH_MAX */
#include "syscall_proc.h"
#include "proc_emulation.h"
#include "hv_util.h"
#include "syscall_io.h"

/* ------------------------------------------------------------------ */
/*  Phase 1: rosetta_prepare — before page table build                */
/* ------------------------------------------------------------------ */

int rosetta_prepare(guest_t *g, const char *binary_path,
                    mem_region_t *regions, int *nregions, int max_regions,
                    int verbose, rosetta_result_t *result) {
    (void)binary_path;  /* used only by rosetta_finalize */
    memset(result, 0, sizeof(*result));

    /* ---- Load and parse rosetta ELF ---- */
    if (access(ROSETTA_PATH, X_OK) != 0) {
        fprintf(stderr, "hl: x86_64 binary requires Rosetta Linux translator\n"
                "    not found at: %s\n"
                "    install Rosetta via: softwareupdate --install-rosetta\n",
                ROSETTA_PATH);
        return -1;
    }
    if (elf_load(ROSETTA_PATH, &result->rosetta_info) < 0) {
        fprintf(stderr, "hl: failed to load rosetta: %s\n", ROSETTA_PATH);
        return -1;
    }
    if (verbose)
        fprintf(stderr, "hl: x86_64 detected, rosetta entry=0x%llx, "
                "load range [0x%llx, 0x%llx)\n",
                (unsigned long long)result->rosetta_info.entry,
                (unsigned long long)result->rosetta_info.load_min,
                (unsigned long long)result->rosetta_info.load_max);

    elf_info_t *ri = &result->rosetta_info;

    /* ---- Compute 2MB-aligned rosetta placement ---- */
    uint64_t va_base = ri->load_min & ~(BLOCK_2MB - 1);
    uint64_t va_end  = (ri->load_max + BLOCK_2MB - 1) & ~(BLOCK_2MB - 1);
    uint64_t size    = va_end - va_base;

    if (g->rosetta_guest_base == 0) {
        /* ---- First-time setup: allocate placement, VA aliases, kbuf ---- */

        /* Place just below interp_base, 2MB-aligned */
        uint64_t guest_base = (g->interp_base - size) & ~(BLOCK_2MB - 1);

        /* Load rosetta segments into primary buffer.  The load_base offsets
         * the high VAs down to the guest base offset:
         * e.g., 0x800000000000 → rosetta_guest_base. */
        uint64_t load_base = guest_base - va_base;
        if (elf_map_segments(ri, ROSETTA_PATH,
                             g->host_base, g->guest_size, load_base) < 0) {
            fprintf(stderr, "hl: failed to map rosetta segments\n");
            return -1;
        }

        /* Register VA alias so syscall handlers can resolve rosetta's
         * high virtual addresses (0x800000000000+) to host pointers. */
        if (guest_add_va_alias(g, va_base, guest_base, size) < 0) {
            fprintf(stderr, "hl: failed to register rosetta VA alias\n");
            return -1;
        }

        if (verbose)
            fprintf(stderr, "hl: rosetta loaded at GPA 0x%llx (VA 0x%llx), "
                    "%lluMB\n",
                    (unsigned long long)guest_base,
                    (unsigned long long)va_base,
                    (unsigned long long)(size / (1024 * 1024)));

        /* Initialize kbuf (kernel VA backing store) for rosetta. */
        uint64_t kbuf_gpa = (guest_base + size + BLOCK_2MB - 1)
                            & ~(BLOCK_2MB - 1);
        if (guest_init_kbuf(g, kbuf_gpa) < 0) {
            fprintf(stderr, "hl: failed to initialize kbuf\n");
            return -1;
        }
        /* Register user VA alias for kbuf so syscall handlers can
         * resolve the user VA addresses we return from kernel VA mmaps. */
        guest_add_va_alias(g, KBUF_USER_VA, kbuf_gpa, KBUF_SIZE);

        /* Persist placement in guest_t for execve re-setup */
        g->rosetta_guest_base = guest_base;
        g->rosetta_va_base    = va_base;
        g->rosetta_size       = size;
    } else {
        /* ---- Re-setup (after execve): reload segments into existing
         *      placement.  guest_reset() zeroed the data AND the PT pool,
         *      so TTBR1 page tables must be rebuilt.  VA aliases and the
         *      kbuf host pointer survive. ---- */
        uint64_t guest_base = g->rosetta_guest_base;
        uint64_t load_base  = guest_base - va_base;

        if (elf_map_segments(ri, ROSETTA_PATH,
                             g->host_base, g->guest_size, load_base) < 0) {
            fprintf(stderr, "hl: failed to reload rosetta segments\n");
            return -1;
        }

        /* Zero kbuf data region — rosetta reinitializes it at startup */
        if (g->kbuf_base)
            memset(g->kbuf_base, 0, KBUF_SIZE);

        /* Rebuild TTBR1 page tables from the reset PT pool.
         * guest_reset() zeroed pt_pool_next back to PT_POOL_BASE, so
         * calling guest_init_kbuf() here allocates fresh L0/L1/L2 pages
         * BEFORE guest_build_page_tables() allocates TTBR0 pages.
         * Without this, g->ttbr1 would point at stale/overwritten data
         * and any kernel VA access would fault (rc=128). */
        if (guest_init_kbuf(g, g->kbuf_gpa) < 0) {
            fprintf(stderr, "hl: failed to rebuild kbuf page tables\n");
            return -1;
        }

        if (verbose)
            fprintf(stderr, "hl: rosetta reloaded at GPA 0x%llx (VA 0x%llx), "
                    "%lluMB\n",
                    (unsigned long long)guest_base,
                    (unsigned long long)va_base,
                    (unsigned long long)(size / (1024 * 1024)));
    }

    /* ---- I-cache invalidation for rosetta code segments ---- */
    uint64_t guest_base = g->rosetta_guest_base;
    uint64_t r_va_base  = g->rosetta_va_base;
    for (int i = 0; i < ri->num_segments; i++) {
        if (ri->segments[i].flags & PF_X) {
            uint64_t seg_gpa = guest_base +
                               (ri->segments[i].gpa - r_va_base);
            void *host_addr = (uint8_t *)g->host_base + seg_gpa;
            sys_icache_invalidate(host_addr, ri->segments[i].memsz);
        }
    }

    /* ---- Append page table regions ---- */

    /* vDSO page (RX): mapped at VDSO_BASE (0xF000) — shares the first
     * 2MB block with the shim (both RX, permissions merge correctly). */
    if (*nregions < max_regions) {
        regions[(*nregions)++] = (mem_region_t){
            .gpa_start = VDSO_BASE,
            .gpa_end   = VDSO_BASE + VDSO_SIZE,
            .perms     = MEM_PERM_RX
        };
    }

    /* Rosetta segments: non-identity mapping.  VA is at 128TB (rosetta's
     * link address), but data lives in the primary buffer at
     * rosetta_guest_base.  va_base directs page table index computation. */
    for (int i = 0; i < ri->num_segments; i++) {
        if (*nregions >= max_regions) break;
        int perms = MEM_PERM_R;
        if (ri->segments[i].flags & PF_W) perms |= MEM_PERM_W;
        if (ri->segments[i].flags & PF_X) perms |= MEM_PERM_X;
        uint64_t seg_gpa = guest_base +
                           (ri->segments[i].gpa - r_va_base);
        regions[(*nregions)++] = (mem_region_t){
            .gpa_start = seg_gpa,
            .gpa_end   = seg_gpa + ri->segments[i].memsz,
            .va_base   = ri->segments[i].gpa,  /* High VA */
            .perms     = perms
        };
    }

    /* Record entry point (high VA, as-is from the ELF) */
    result->entry_point = ri->entry;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Phase 2: rosetta_finalize — after page table build                */
/* ------------------------------------------------------------------ */

int rosetta_finalize(guest_t *g, hv_vcpu_t vcpu,
                     const char *binary_path,
                     int guest_argc, const char **guest_argv,
                     const rosetta_result_t *rr,
                     int verbose,
                     int *out_argc, const char ***out_argv,
                     uint64_t *out_vdso_addr) {
    /* ---- kbuf user VA dual-mapping (TTBR0) ---- */
    if (g->kbuf_base) {
        if (guest_map_va_range(g, KBUF_USER_VA,
                               KBUF_USER_VA + KBUF_SIZE,
                               g->kbuf_gpa, MEM_PERM_RW) < 0) {
            fprintf(stderr, "hl: failed to create kbuf user VA mapping\n");
            return -1;
        }
    }

    /* ---- TCR_EL1: enable TTBR1 walks + TBI1 for kernel VA ---- */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1,
                                  TCR_EL1_VALUE_KBUF));

    /* ---- TTBR1_EL1: kernel VA page tables ---- */
    if (g->ttbr1)
        HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR1_EL1, g->ttbr1));

    /* ---- Build vDSO ---- */
    uint64_t vdso_addr = vdso_build(g);
    if (!vdso_addr) {
        fprintf(stderr, "hl: failed to build vDSO\n");
        return -1;
    }
    /* Register [vvar] and [vdso] in the region table */
    guest_region_add(g, VDSO_BASE - 0x1000, VDSO_BASE,
                     LINUX_PROT_READ, LINUX_MAP_PRIVATE, 0, "[vvar]");
    guest_region_add(g, VDSO_BASE, VDSO_BASE + VDSO_SIZE,
                     LINUX_PROT_READ | LINUX_PROT_EXEC,
                     LINUX_MAP_PRIVATE, 0, "[vdso]");
    if (verbose)
        fprintf(stderr, "hl: vDSO built at 0x%llx\n",
                (unsigned long long)vdso_addr);

    /* ---- Rosetta semantic regions for /proc/self/maps ---- */
    const elf_info_t *ri = &rr->rosetta_info;
    for (int i = 0; i < ri->num_segments; i++) {
        int prot = LINUX_PROT_READ;
        if (ri->segments[i].flags & PF_W) prot |= LINUX_PROT_WRITE;
        if (ri->segments[i].flags & PF_X) prot |= LINUX_PROT_EXEC;
        guest_region_add(g, ri->segments[i].gpa,
                         ri->segments[i].gpa + ri->segments[i].memsz,
                         prot, LINUX_MAP_PRIVATE,
                         ri->segments[i].offset, ROSETTA_PATH);
    }

    /* ---- Pre-open binary at fd 3 for rosetta ---- */
    int bin_host_fd = open(binary_path, O_RDONLY);
    if (bin_host_fd < 0) {
        fprintf(stderr, "hl: failed to pre-open binary: %s\n", binary_path);
        return -1;
    }
    int bin_guest_fd = fd_alloc_at(3, FD_REGULAR, bin_host_fd);
    if (bin_guest_fd < 0) {
        fprintf(stderr, "hl: failed to allocate fd 3 for binary\n");
        close(bin_host_fd);
        return -1;
    }

    /* ---- Build binfmt_misc argv ---- */
    /* rosetta_argv = [rosetta_path, binary_path, guest_argv[1:], NULL]
     * Minimum 2 entries even when guest_argc == 0 (no argv[0]). */
    int rosetta_argc = (guest_argc > 0) ? guest_argc + 1 : 2;
    const char **rosetta_argv = malloc(sizeof(char *) * (rosetta_argc + 1));
    if (!rosetta_argv) {
        fprintf(stderr, "hl: malloc failed for rosetta argv\n");
        return -1;
    }
    rosetta_argv[0] = ROSETTA_PATH;
    rosetta_argv[1] = binary_path;
    for (int i = 1; i < guest_argc; i++)
        rosetta_argv[i + 1] = guest_argv[i];
    rosetta_argv[rosetta_argc] = NULL;

    /* ---- Set proc state ---- */
    proc_set_elf_path(ROSETTA_PATH);
    rosettad_set_binary_path(binary_path);
    proc_set_cmdline(rosetta_argc, rosetta_argv);

    /* ---- Output ---- */
    *out_argc     = rosetta_argc;
    *out_argv     = rosetta_argv;
    *out_vdso_addr = vdso_addr;

    return 0;
}
