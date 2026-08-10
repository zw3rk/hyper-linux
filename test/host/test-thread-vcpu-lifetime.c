/* test-thread-vcpu-lifetime.c — vCPU handle ownership regression tests
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "thread.h"

#include <stdio.h>

static int failures;
static int destroy_calls;
static int interrupt_calls;
static hv_vcpu_t destroyed_vcpu;
static hv_vcpu_t interrupted_vcpu;

hv_return_t test_hv_vcpu_destroy(hv_vcpu_t vcpu) {
    destroy_calls++;
    destroyed_vcpu = vcpu;
    return HV_SUCCESS;
}

hv_return_t test_hv_vcpus_exit(hv_vcpu_t *vcpus, uint32_t count) {
    interrupt_calls++;
    if (count == 1)
        interrupted_vcpu = vcpus[0];
    return HV_SUCCESS;
}

static void check(int condition, const char *message) {
    if (condition) {
        printf("PASS: %s\n", message);
    } else {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    thread_init();
    thread_register_main(0, NULL, 1, 0);

    check(current_thread != NULL && current_thread->vcpu_valid,
          "publish valid vCPU handle zero");
    check(thread_interrupt_tid(1) == 1 && interrupt_calls == 1 &&
          interrupted_vcpu == 0,
          "interrupt valid vCPU handle zero");
    check(thread_retire_vcpu(current_thread) == 1 && destroy_calls == 1 &&
          destroyed_vcpu == 0,
          "destroy valid vCPU handle zero");
    check(current_thread->vcpu_valid == 0,
          "remove retired handle from interrupt scans");
    thread_interrupt_all();
    check(interrupt_calls == 1,
          "do not interrupt a retired vCPU");
    check(thread_retire_vcpu(current_thread) == 0 && destroy_calls == 1,
          "do not destroy a retired vCPU twice");

    thread_deactivate(current_thread);
    return failures ? 1 : 0;
}
