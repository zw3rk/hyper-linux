/* crash_report.c — Structured crash report for GitHub issue filing
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Prints a structured diagnostic to stderr when hl encounters a fatal
 * error. Sections: environment, crash type, binary info, registers,
 * memory layout, and instructions for filing a GitHub issue.
 */
#include "crash_report.h"
#include "syscall_proc.h"
#include "version.h"

#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Read a sysctl string into buf (NUL-terminated). Returns 0 on success. */
static int sysctl_str(const char *name, char *buf, size_t bufsz) {
    size_t len = bufsz;
    if (sysctlbyname(name, buf, &len, NULL, 0) != 0) {
        buf[0] = '\0';
        return -1;
    }
    buf[bufsz - 1] = '\0';
    return 0;
}

/* Map crash_type_t to a human-readable label. */
static const char *crash_type_name(crash_type_t type) {
    switch (type) {
    case CRASH_TIMEOUT:         return "TIMEOUT";
    case CRASH_BAD_EXCEPTION:   return "BAD_EXCEPTION";
    case CRASH_UNEXPECTED_HVC:  return "UNEXPECTED_HVC";
    case CRASH_UNEXPECTED_EC:   return "UNEXPECTED_EC";
    case CRASH_UNEXPECTED_EXIT: return "UNEXPECTED_EXIT";
    case CRASH_ELR_ZERO:        return "ELR_ZERO";
    case CRASH_HV_CHECK:        return "HV_CHECK";
    }
    return "UNKNOWN";
}

/* ── Public API ───────────────────────────────────────────────────── */

void crash_report(hv_vcpu_t vcpu, const guest_t *g,
                  crash_type_t type, const char *detail) {

    fprintf(stderr,
        "\n╔══════════════════════════════════════════════════════════╗\n"
          "║                   hl crash report                        ║\n"
          "╚══════════════════════════════════════════════════════════╝\n\n");

    /* ── 1. Environment ─────────────────────────────────────────── */
    char os_version[64] = {0};
    char os_release[64] = {0};
    char hw_model[128]  = {0};

    sysctl_str("kern.osproductversion", os_version, sizeof(os_version));
    sysctl_str("kern.osrelease",        os_release, sizeof(os_release));
    /* machdep.cpu.brand_string is x86-only; hw.model works on both */
    if (sysctl_str("machdep.cpu.brand_string", hw_model, sizeof(hw_model)) != 0)
        sysctl_str("hw.model", hw_model, sizeof(hw_model));

    fprintf(stderr, "## Environment\n");
    fprintf(stderr, "- hl version: %s\n", HL_VERSION);
    fprintf(stderr, "- macOS: %s (Darwin %s)\n",
            os_version[0] ? os_version : "?",
            os_release[0] ? os_release : "?");
    fprintf(stderr, "- hardware: %s\n\n",
            hw_model[0] ? hw_model : "Apple Silicon");

    /* ── 2. Crash type ──────────────────────────────────────────── */
    fprintf(stderr, "## Crash\n");
    fprintf(stderr, "- type: %s\n", crash_type_name(type));
    if (detail && detail[0])
        fprintf(stderr, "- detail: %s\n", detail);
    fprintf(stderr, "\n");

    /* ── 3. Binary info ─────────────────────────────────────────── */
    const char *elf_path = proc_get_elf_path();
    size_t cmdline_len = 0;
    const char *cmdline = proc_get_cmdline(&cmdline_len);
    const char *sysroot = proc_get_sysroot();

    fprintf(stderr, "## Binary\n");
    fprintf(stderr, "- path: %s\n", elf_path ? elf_path : "(unknown)");
    if (sysroot)
        fprintf(stderr, "- sysroot: %s\n", sysroot);

    /* Print command line (NUL-separated → space-separated) */
    if (cmdline && cmdline_len > 0) {
        fprintf(stderr, "- cmdline:");
        size_t pos = 0;
        while (pos < cmdline_len) {
            fprintf(stderr, " %s", cmdline + pos);
            pos += strlen(cmdline + pos) + 1;
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");

    /* ── 4. Registers ───────────────────────────────────────────── */
    if (vcpu) {
        fprintf(stderr, "## Registers\n");
        fprintf(stderr, "```\n");

        uint64_t pc = 0, cpsr = 0;
        hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
        hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
        fprintf(stderr, "PC   = 0x%016llx  CPSR = 0x%016llx\n",
                (unsigned long long)pc, (unsigned long long)cpsr);

        uint64_t esr = 0, far_reg = 0, elr = 0, spsr = 0, sctlr = 0, sp_el0 = 0;
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1,   &esr);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1,   &far_reg);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1,   &elr);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1,  &spsr);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr);
        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0,    &sp_el0);

        fprintf(stderr, "ESR  = 0x%016llx  FAR  = 0x%016llx\n",
                (unsigned long long)esr, (unsigned long long)far_reg);
        fprintf(stderr, "ELR  = 0x%016llx  SPSR = 0x%016llx\n",
                (unsigned long long)elr, (unsigned long long)spsr);
        fprintf(stderr, "SCTLR= 0x%016llx  SP0  = 0x%016llx\n",
                (unsigned long long)sctlr, (unsigned long long)sp_el0);
        fprintf(stderr, "\n");

        /* GPRs X0-X30 (4 per line) */
        for (int i = 0; i <= 30; i++) {
            uint64_t val = 0;
            hv_vcpu_get_reg(vcpu, (hv_reg_t)(HV_REG_X0 + i), &val);
            fprintf(stderr, "X%-2d  = 0x%016llx", i, (unsigned long long)val);
            if ((i % 4) == 3 || i == 30)
                fprintf(stderr, "\n");
            else
                fprintf(stderr, "  ");
        }
        fprintf(stderr, "```\n\n");
    }

    /* ── 5. Memory layout ───────────────────────────────────────── */
    if (g) {
        fprintf(stderr, "## Memory layout\n");
        fprintf(stderr, "```\n");
        fprintf(stderr, "guest_size    = 0x%llx (%llu MB)\n",
                (unsigned long long)g->guest_size,
                (unsigned long long)(g->guest_size >> 20));
        fprintf(stderr, "ipa_bits      = %u\n", g->ipa_bits);
        fprintf(stderr, "brk           = 0x%llx .. 0x%llx\n",
                (unsigned long long)g->brk_base,
                (unsigned long long)g->brk_current);
        fprintf(stderr, "mmap RW       = 0x%llx .. 0x%llx (next 0x%llx)\n",
                (unsigned long long)MMAP_BASE,
                (unsigned long long)g->mmap_end,
                (unsigned long long)g->mmap_next);
        fprintf(stderr, "mmap RX       = 0x%llx .. 0x%llx (next 0x%llx)\n",
                (unsigned long long)MMAP_RX_BASE,
                (unsigned long long)g->mmap_rx_end,
                (unsigned long long)g->mmap_rx_next);
        fprintf(stderr, "interp_base   = 0x%llx\n",
                (unsigned long long)g->interp_base);
        fprintf(stderr, "mmap_limit    = 0x%llx\n",
                (unsigned long long)g->mmap_limit);
        fprintf(stderr, "nregions      = %d\n", g->nregions);
        fprintf(stderr, "```\n\n");
    }

    /* ── 6. How to report ───────────────────────────────────────── */
    fprintf(stderr,
        "## How to report\n"
        "1. Copy everything above into a new issue at:\n"
        "   https://github.com/zw3rk/hyper-linux/issues/new\n"
        "2. Attach the binary that triggered the crash (if possible).\n"
        "3. Include the full command line used to invoke hl.\n\n");
}
