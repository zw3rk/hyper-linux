/* crash_report.h — Structured crash report for GitHub issue filing
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * When hl encounters a fatal error (timeout, bad exception, unexpected
 * HVC, etc.), crash_report() prints a structured diagnostic to stderr
 * formatted as a GitHub issue template. This gives users all the info
 * they need to file a useful bug report.
 */
#ifndef CRASH_REPORT_H
#define CRASH_REPORT_H

#include <Hypervisor/Hypervisor.h>
#include "guest.h"

/* Crash type classification for the report header. */
typedef enum {
    CRASH_TIMEOUT,          /* vCPU execution timed out (alarm) */
    CRASH_BAD_EXCEPTION,    /* HVC #2: guest exception at EL1 */
    CRASH_UNEXPECTED_HVC,   /* Unknown HVC immediate value */
    CRASH_UNEXPECTED_EC,    /* Unhandled exception class at EL2 */
    CRASH_UNEXPECTED_EXIT,  /* hv_vcpu_run returned unknown reason */
    CRASH_ELR_ZERO,         /* ELR_EL1=0 after exec (register sync bug) */
    CRASH_HV_CHECK,         /* Hypervisor.framework API call failed */
} crash_type_t;

/* Print a structured crash report to stderr.
 *
 * vcpu: vCPU handle for register dump (0 if unavailable).
 * g: guest state for memory layout dump (NULL if unavailable).
 * type: crash classification.
 * detail: human-readable context string (e.g., "HVC #99", "EC=0x20"). */
void crash_report(hv_vcpu_t vcpu, const guest_t *g,
                  crash_type_t type, const char *detail);

#endif /* CRASH_REPORT_H */
