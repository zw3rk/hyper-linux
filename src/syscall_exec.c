/* syscall_exec.c — execve syscall handler for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements execve: reads path/argv/envp from guest memory, closes
 * CLOEXEC fds, resets the guest VM, reloads the shim and new ELF,
 * rebuilds page tables, and restarts at the new entry point.
 */
#include "syscall_exec.h"
#include "syscall_proc.h"    /* proc_set_elf_path, proc_get_shim_blob, proc_get_shim_size, SYSCALL_EXEC_HAPPENED */
#include "syscall_internal.h" /* fd_table, FD_TABLE_SIZE */
#include "syscall.h"         /* FD_CLOSED, FD_STDIO, LINUX_O_CLOEXEC, etc. */
#include "guest.h"           /* guest_t, guest_reset, guest_build_page_tables, etc. */
#include "elf.h"             /* elf_load, elf_map_segments */
#include "syscall_signal.h"  /* signal_reset_for_exec */
#include "stack.h"           /* build_linux_stack */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <libkern/OSCacheControl.h>

/* Read a NULL-terminated pointer array from guest memory.
 * Each pointer in the array is a 64-bit GVA pointing to a string.
 * Returns the count of entries (excluding the NULL terminator),
 * or -1 on error. Strings are copied into the provided buffer. */
static int read_string_array(guest_t *g, uint64_t array_gva,
                             char **out, int max_count,
                             char *str_buf, size_t str_buf_size) {
    size_t str_off = 0;
    int count = 0;

    for (int i = 0; i < max_count; i++) {
        uint64_t ptr;
        if (guest_read(g, array_gva + i * 8, &ptr, 8) < 0)
            return -1;
        if (ptr == 0) break; /* NULL terminator */

        char *dst = str_buf + str_off;
        size_t remaining = str_buf_size - str_off;
        if (remaining < 2) return -1; /* Buffer full */

        if (guest_read_str(g, ptr, dst, remaining) < 0)
            return -1;

        out[count] = dst;
        str_off += strlen(dst) + 1;
        count++;
    }

    return count;
}

int64_t sys_execve(hv_vcpu_t vcpu, guest_t *g,
                   uint64_t path_gva, uint64_t argv_gva, uint64_t envp_gva,
                   int verbose) {
    /* Step 1: Read path from guest memory */
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    if (verbose)
        fprintf(stderr, "hl: execve(\"%s\")\n", path);

    /* Step 2: Read argv[] and envp[] from guest memory */
    #define MAX_ARGS 256
    #define MAX_ENVS 4096
    #define STR_BUF_SIZE (256 * 1024)

    char *argv[MAX_ARGS + 1];
    char *envp[MAX_ENVS + 1];
    char *argv_buf = malloc(STR_BUF_SIZE);
    char *envp_buf = malloc(STR_BUF_SIZE);
    if (!argv_buf || !envp_buf) {
        free(argv_buf);
        free(envp_buf);
        return -LINUX_ENOMEM;
    }

    int argc = read_string_array(g, argv_gva, argv, MAX_ARGS,
                                  argv_buf, STR_BUF_SIZE);
    if (argc < 0) {
        free(argv_buf); free(envp_buf);
        return -LINUX_EFAULT;
    }
    argv[argc] = NULL;

    int envc = 0;
    if (envp_gva != 0) {
        envc = read_string_array(g, envp_gva, envp, MAX_ENVS,
                                  envp_buf, STR_BUF_SIZE);
        if (envc < 0) {
            free(argv_buf); free(envp_buf);
            return -LINUX_EFAULT;
        }
    }
    envp[envc] = NULL;

    /* Step 3: Try loading as ELF; if that fails, check for shebang (#!).
     * Linux kernel handles shebangs transparently in binfmt_script. */
    elf_info_t elf_info;
    if (elf_load(path, &elf_info) < 0) {
        /* Not a valid ELF — check if it's a script with a shebang line.
         * Read the first 256 bytes and look for "#!" at the start. */
        int script_fd = open(path, O_RDONLY);
        if (script_fd < 0) {
            free(argv_buf); free(envp_buf);
            return -LINUX_ENOENT;
        }
        char shebang_buf[256];
        ssize_t nread = read(script_fd, shebang_buf, sizeof(shebang_buf) - 1);
        close(script_fd);

        if (nread < 2 || shebang_buf[0] != '#' || shebang_buf[1] != '!') {
            free(argv_buf); free(envp_buf);
            return -LINUX_ENOEXEC;
        }
        shebang_buf[nread] = '\0';

        /* Find end of the shebang line */
        char *eol = strchr(shebang_buf + 2, '\n');
        if (eol) *eol = '\0';

        /* Parse interpreter path and optional argument.
         * Format: "#! /path/to/interpreter [optional-arg]" */
        char *interp_start = shebang_buf + 2;
        while (*interp_start == ' ' || *interp_start == '\t')
            interp_start++;
        if (*interp_start == '\0') {
            free(argv_buf); free(envp_buf);
            return -LINUX_ENOEXEC;
        }

        /* Split into interpreter path and optional argument */
        char *interp_arg = NULL;
        char *space = interp_start;
        while (*space && *space != ' ' && *space != '\t')
            space++;
        if (*space) {
            *space = '\0';
            interp_arg = space + 1;
            while (*interp_arg == ' ' || *interp_arg == '\t')
                interp_arg++;
            if (*interp_arg == '\0') interp_arg = NULL;
            /* Trim trailing whitespace from arg */
            if (interp_arg) {
                char *end = interp_arg + strlen(interp_arg) - 1;
                while (end > interp_arg && (*end == ' ' || *end == '\t' || *end == '\r'))
                    *end-- = '\0';
            }
        }

        if (verbose)
            fprintf(stderr, "hl: execve: shebang interp=\"%s\" arg=\"%s\" script=\"%s\"\n",
                    interp_start, interp_arg ? interp_arg : "(none)", path);

        /* Rebuild argv: [interpreter, optional-arg, script-path, original-argv[1:]] */
        int new_argc = 1 + (interp_arg ? 1 : 0) + 1 + (argc > 1 ? argc - 1 : 0);
        if (new_argc > MAX_ARGS) {
            free(argv_buf); free(envp_buf);
            return -LINUX_E2BIG;
        }

        /* Allocate new argv on the stack (small, bounded) */
        char **new_argv = alloca((new_argc + 1) * sizeof(char *));
        int ni = 0;
        new_argv[ni++] = interp_start;
        if (interp_arg) new_argv[ni++] = interp_arg;
        new_argv[ni++] = path;
        for (int i = 1; i < argc; i++)
            new_argv[ni++] = argv[i];
        new_argv[ni] = NULL;

        /* Copy new argv into argv_buf for the recursive call */
        size_t buf_off = 0;
        for (int i = 0; i < ni; i++) {
            size_t len = strlen(new_argv[i]);
            if (buf_off + len + 1 > STR_BUF_SIZE) {
                free(argv_buf); free(envp_buf);
                return -LINUX_E2BIG;
            }
            memcpy(argv_buf + buf_off, new_argv[i], len + 1);
            argv[i] = argv_buf + buf_off;
            buf_off += len + 1;
        }
        argv[ni] = NULL;
        argc = ni;

        /* Replace path with interpreter and re-try ELF load */
        strncpy(path, interp_start, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';

        if (elf_load(path, &elf_info) < 0) {
            free(argv_buf); free(envp_buf);
            return -LINUX_ENOENT;
        }
    }

    /* Step 4: Close CLOEXEC fds */
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fd_table[i].type != FD_CLOSED &&
            (fd_table[i].linux_flags & LINUX_O_CLOEXEC)) {
            if (fd_table[i].dir) {
                closedir((DIR *)fd_table[i].dir);
                fd_table[i].dir = NULL;
            }
            if (fd_table[i].type != FD_STDIO)
                close(fd_table[i].host_fd);
            fd_mark_closed(i);
            fd_table[i].host_fd = -1;
            fd_table[i].linux_flags = 0;
        }
    }

    /* Step 5: Reset guest memory (zero ELF, brk, stack, mmap regions) */
    guest_reset(g);

    /* Step 5b: Reset signal state for exec (POSIX requirement).
     * Handlers set to SIG_DFL (except SIG_IGN stays SIG_IGN),
     * pending signals preserved, signal mask preserved. */
    signal_reset_for_exec();

    /* Step 6: Reload shim into guest */
    const unsigned char *shim_ptr = proc_get_shim_blob();
    unsigned int shim_size = proc_get_shim_size();
    if (shim_ptr && shim_size > 0) {
        memcpy((uint8_t *)g->host_base + SHIM_BASE, shim_ptr, shim_size);
    }

    /* Step 7: Load new ELF segments into guest memory.
     * PIE (ET_DYN) binaries start near address 0 and would overlap with
     * the shim; load them at PIE_LOAD_BASE instead. */
    uint64_t elf_load_base = (elf_info.e_type == ET_DYN) ? PIE_LOAD_BASE : 0;
    if (elf_map_segments(&elf_info, path, g->host_base, g->guest_size,
                         elf_load_base) < 0) {
        fprintf(stderr, "hl: execve: failed to map ELF segments for %s\n", path);
        free(argv_buf); free(envp_buf);
        return -LINUX_ENOEXEC;
    }

    /* Step 7b: Load interpreter if the new binary is dynamically linked */
    elf_info_t interp_info;
    uint64_t interp_base = 0;
    memset(&interp_info, 0, sizeof(interp_info));

    if (elf_info.interp_path[0] != '\0') {
        const char *sr = proc_get_sysroot();
        char interp_resolved[LINUX_PATH_MAX];
        elf_resolve_interp(sr, elf_info.interp_path,
                           interp_resolved, sizeof(interp_resolved));

        if (verbose)
            fprintf(stderr, "hl: execve: loading interpreter: %s\n",
                    interp_resolved);

        if (elf_load(interp_resolved, &interp_info) < 0) {
            fprintf(stderr, "hl: execve: failed to load interpreter: %s\n",
                    interp_resolved);
            free(argv_buf); free(envp_buf);
            return -LINUX_ENOEXEC;
        }

        interp_base = g->interp_base;
        if (elf_map_segments(&interp_info, interp_resolved,
                             g->host_base, g->guest_size, interp_base) < 0) {
            fprintf(stderr, "hl: execve: failed to map interpreter segments\n");
            free(argv_buf); free(envp_buf);
            return -LINUX_ENOEXEC;
        }

        if (verbose) {
            fprintf(stderr, "hl: execve: interpreter at base=0x%llx, "
                    "entry=0x%llx, %d segments\n",
                    (unsigned long long)interp_base,
                    (unsigned long long)(interp_info.entry + interp_base),
                    interp_info.num_segments);
        }
    }

    /* Step 7c: Invalidate I-cache for loaded code regions.
     * ARM64 I-cache and D-cache are not coherent. elf_map_segments writes
     * code via memcpy (updating D-cache), but the I-cache may still hold
     * stale instructions from the pre-exec binary at the same addresses.
     * Without explicit invalidation, the vCPU executes wrong instructions
     * after ERET — this was the root cause of flaky test-fork-exec failures
     * (stale I-cache from the pre-exec binary persisted non-deterministically,
     * depending on cache eviction pressure from intervening memory accesses). */
    for (int i = 0; i < elf_info.num_segments; i++) {
        if (elf_info.segments[i].flags & PF_X) {
            void *host_addr = (uint8_t *)g->host_base +
                              elf_info.segments[i].gpa + elf_load_base;
            sys_icache_invalidate(host_addr, elf_info.segments[i].memsz);
        }
    }
    for (int i = 0; i < interp_info.num_segments; i++) {
        if (interp_info.segments[i].flags & PF_X) {
            void *host_addr = (uint8_t *)g->host_base +
                              interp_info.segments[i].gpa + interp_base;
            sys_icache_invalidate(host_addr, interp_info.segments[i].memsz);
        }
    }
    /* Also invalidate the reloaded shim code — I-cache lines may have been
     * evicted and refilled with zeroes during guest_reset(). */
    sys_icache_invalidate((uint8_t *)g->host_base + SHIM_BASE, shim_size);

    /* Set brk base after the highest loaded segment */
    uint64_t brk_start = (elf_info.load_max + elf_load_base + 4095) & ~4095ULL;
    if (brk_start < BRK_BASE_DEFAULT)
        brk_start = BRK_BASE_DEFAULT;
    g->brk_base = brk_start;
    g->brk_current = brk_start;

    /* Step 8: Rebuild page tables */
    #define MAX_REGIONS 32
    mem_region_t regions[MAX_REGIONS];
    int nregions = 0;

    /* Shim code (RX) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = SHIM_BASE,
        .gpa_end   = SHIM_BASE + shim_size,
        .perms     = MEM_PERM_RX
    };

    /* Shim data/stack (RW) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = SHIM_DATA_BASE,
        .gpa_end   = SHIM_DATA_BASE + BLOCK_2MB,
        .perms     = MEM_PERM_RW
    };

    /* ELF segments (adjusted by elf_load_base for PIE) */
    for (int i = 0; i < elf_info.num_segments; i++) {
        if (nregions >= MAX_REGIONS) break;
        int perms = MEM_PERM_R;
        if (elf_info.segments[i].flags & PF_W) perms |= MEM_PERM_W;
        if (elf_info.segments[i].flags & PF_X) perms |= MEM_PERM_X;
        regions[nregions++] = (mem_region_t){
            .gpa_start = elf_info.segments[i].gpa + elf_load_base,
            .gpa_end   = elf_info.segments[i].gpa + elf_info.segments[i].memsz + elf_load_base,
            .perms     = perms
        };
    }

    /* Interpreter segments (if dynamically linked) */
    for (int i = 0; i < interp_info.num_segments; i++) {
        if (nregions >= MAX_REGIONS) break;
        int perms = MEM_PERM_R;
        if (interp_info.segments[i].flags & PF_W) perms |= MEM_PERM_W;
        if (interp_info.segments[i].flags & PF_X) perms |= MEM_PERM_X;
        regions[nregions++] = (mem_region_t){
            .gpa_start = interp_info.segments[i].gpa + interp_base,
            .gpa_end   = interp_info.segments[i].gpa + interp_info.segments[i].memsz + interp_base,
            .perms     = perms
        };
    }

    /* brk region (RW). Pre-mapped up to MMAP_RX_BASE. */
    regions[nregions++] = (mem_region_t){
        .gpa_start = g->brk_base,
        .gpa_end   = MMAP_RX_BASE,
        .perms     = MEM_PERM_RW
    };

    /* Stack (RW) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = STACK_BASE,
        .gpa_end   = STACK_TOP,
        .perms     = MEM_PERM_RW
    };

    /* mmap RX region (for PROT_EXEC allocations) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = MMAP_RX_BASE,
        .gpa_end   = MMAP_RX_INITIAL_END,
        .perms     = MEM_PERM_RX
    };
    g->mmap_rx_end = MMAP_RX_INITIAL_END;

    /* mmap RW region (starts at 8GB to match real Linux layout) */
    regions[nregions++] = (mem_region_t){
        .gpa_start = MMAP_BASE,
        .gpa_end   = MMAP_INITIAL_END,
        .perms     = MEM_PERM_RW
    };
    g->mmap_end = MMAP_INITIAL_END;

    uint64_t ttbr0 = guest_build_page_tables(g, regions, nregions);
    if (!ttbr0) {
        fprintf(stderr, "hl: execve: failed to build page tables\n");
        free(argv_buf); free(envp_buf);
        return -LINUX_ENOMEM;
    }

    /* Step 8b: Record semantic regions (cleared by guest_reset) */
    guest_region_add(g, SHIM_BASE, SHIM_BASE + shim_size,
                     LINUX_PROT_READ | LINUX_PROT_EXEC, LINUX_MAP_PRIVATE,
                     0, "[shim]");
    guest_region_add(g, SHIM_DATA_BASE, SHIM_DATA_BASE + BLOCK_2MB,
                     LINUX_PROT_READ | LINUX_PROT_WRITE, LINUX_MAP_PRIVATE,
                     0, "[shim-data]");
    for (int i = 0; i < elf_info.num_segments; i++) {
        int seg_prot = LINUX_PROT_READ;
        if (elf_info.segments[i].flags & PF_W) seg_prot |= LINUX_PROT_WRITE;
        if (elf_info.segments[i].flags & PF_X) seg_prot |= LINUX_PROT_EXEC;
        guest_region_add(g, elf_info.segments[i].gpa + elf_load_base,
                         elf_info.segments[i].gpa + elf_info.segments[i].memsz + elf_load_base,
                         seg_prot, LINUX_MAP_PRIVATE,
                         elf_info.segments[i].offset, path);
    }
    /* Interpreter semantic regions */
    if (interp_info.num_segments > 0) {
        char interp_resolved[LINUX_PATH_MAX];
        elf_resolve_interp(proc_get_sysroot(), elf_info.interp_path,
                           interp_resolved, sizeof(interp_resolved));
        for (int i = 0; i < interp_info.num_segments; i++) {
            int seg_prot = LINUX_PROT_READ;
            if (interp_info.segments[i].flags & PF_W) seg_prot |= LINUX_PROT_WRITE;
            if (interp_info.segments[i].flags & PF_X) seg_prot |= LINUX_PROT_EXEC;
            guest_region_add(g, interp_info.segments[i].gpa + interp_base,
                             interp_info.segments[i].gpa + interp_info.segments[i].memsz + interp_base,
                             seg_prot, LINUX_MAP_PRIVATE,
                             interp_info.segments[i].offset, interp_resolved);
        }
    }
    /* Stack guard page: PROT_NONE at bottom to catch overflow */
    guest_invalidate_ptes(g, STACK_BASE, STACK_BASE + STACK_GUARD_SIZE);
    guest_region_add(g, STACK_BASE, STACK_BASE + STACK_GUARD_SIZE,
                     LINUX_PROT_NONE,
                     LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
                     0, "[stack-guard]");
    guest_region_add(g, STACK_BASE + STACK_GUARD_SIZE, STACK_TOP,
                     LINUX_PROT_READ | LINUX_PROT_WRITE,
                     LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
                     0, "[stack]");

    /* Step 9: Build new stack with new argv/envp.
     * Pass interp_base so AT_BASE is set for the dynamic linker. */
    const char **argv_const = (const char **)argv;
    const char **envp_const = (const char **)envp;
    uint64_t sp = build_linux_stack(g, STACK_TOP, argc, argv_const,
                                     envp_const, &elf_info,
                                     elf_load_base, interp_base,
                                     0 /* no vDSO for execve */);

    /* Step 10: Set vCPU state for new process.
     * Entry point: interpreter if dynamic, ELF entry if static. */
    uint64_t entry_point = (interp_base != 0)
        ? (interp_info.entry + interp_base)
        : (elf_info.entry + elf_load_base);
    uint64_t entry_ipa = guest_ipa(g, entry_point);
    uint64_t sp_ipa    = guest_ipa(g, sp);

    /* Write TTBR0 to vCPU */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, ttbr0);

    /* Set ELR_EL1 to new entry point */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, entry_ipa);

    /* Set SP_EL0 to new stack */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, sp_ipa);

    /* SPSR_EL1: EL0t, AArch64 */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, 0x0);

    /* Zero all general purpose registers */
    for (int i = 0; i < 31; i++) {
        hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, 0);
    }

    /* Tell the shim to invalidate TLB after we rebuilt page tables.
     * Set X8=1 directly because SYSCALL_EXEC_HAPPENED bypasses the
     * normal X8 TLBI signaling in syscall_dispatch(). The shim checks
     * X8 after HVC #5 return: if non-zero, it runs TLBI VMALLE1IS. */
    hv_vcpu_set_reg(vcpu, HV_REG_X8, 1);
    g->need_tlbi = 0;

    /* Force HVF to synchronize all register writes. Apple's Hypervisor
     * framework may batch register updates; reading back each modified
     * register ensures all preceding set_sys_reg/set_reg calls are
     * committed before the next hv_vcpu_run(). Without this, the vCPU
     * may resume with stale ELR_EL1/SPSR_EL1 from the pre-exec state,
     * causing an undefined instruction at address 0x0. */
    {
        uint64_t _sync;
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, &_sync);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &_sync);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &_sync);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, &_sync);
        hv_vcpu_get_reg(vcpu, HV_REG_X8, &_sync);
        (void)_sync;
    }

    if (verbose) {
        /* Read back key registers for debug verification */
        uint64_t dbg_ttbr0, dbg_elr, dbg_sp0, dbg_spsr, dbg_vbar, dbg_sctlr;
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, &dbg_ttbr0);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &dbg_elr);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &dbg_sp0);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, &dbg_spsr);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, &dbg_vbar);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, &dbg_sctlr);
        uint64_t dbg_x8;
        hv_vcpu_get_reg(vcpu, HV_REG_X8, &dbg_x8);

        fprintf(stderr, "hl: execve: loaded %s, entry=0x%llx sp=0x%llx\n",
                path, (unsigned long long)entry_ipa, (unsigned long long)sp_ipa);
        fprintf(stderr, "hl: execve: TTBR0=0x%llx ELR=0x%llx SP0=0x%llx "
                "SPSR=0x%llx VBAR=0x%llx SCTLR=0x%llx X8=%llu\n",
                (unsigned long long)dbg_ttbr0, (unsigned long long)dbg_elr,
                (unsigned long long)dbg_sp0, (unsigned long long)dbg_spsr,
                (unsigned long long)dbg_vbar, (unsigned long long)dbg_sctlr,
                (unsigned long long)dbg_x8);

        /* Dump L2 entries for the entry point's 2MB block */
        uint64_t base = g->ipa_base;
        uint64_t l0_off = dbg_ttbr0 - base;
        uint64_t *l0 = (uint64_t *)((uint8_t *)g->host_base + l0_off);
        if (l0[0] & 1) {
            uint64_t l1_off = (l0[0] & 0xFFFFFFFFF000ULL) - base;
            uint64_t *l1 = (uint64_t *)((uint8_t *)g->host_base + l1_off);
            unsigned l1_idx = (unsigned)(entry_ipa / (1ULL << 30));
            if (l1_idx < 512 && (l1[l1_idx] & 1)) {
                uint64_t l2_off = (l1[l1_idx] & 0xFFFFFFFFF000ULL) - base;
                uint64_t *l2 = (uint64_t *)((uint8_t *)g->host_base + l2_off);
                unsigned l2_idx = (unsigned)((entry_ipa % (1ULL << 30)) / (2ULL << 20));
                fprintf(stderr, "hl: execve: L1[%u]=0x%llx L2[%u]=0x%llx\n",
                        l1_idx, (unsigned long long)l1[l1_idx],
                        l2_idx, (unsigned long long)l2[l2_idx]);
            }
        }
    }

    /* Update ELF path for /proc/self/exe after successful exec */
    proc_set_elf_path(path);

    free(argv_buf);
    free(envp_buf);

    return SYSCALL_EXEC_HAPPENED;
}
