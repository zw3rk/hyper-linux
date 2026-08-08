/* test-page-table-pool.c — host-side page-table capacity regression
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "guest.h"
#include "thread.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

int hl_verbose = 0;

void thread_destroy_all_vcpus(void) {}

int main(void) {
    const uint64_t guest_size = GUEST_MEM_SIZE_36BIT;
    const int blocks_to_split = 512;
    const uint64_t region_start = 0x00400000ULL;
    const uint64_t region_end = region_start + (uint64_t)blocks_to_split * BLOCK_2MB;

    void *memory = mmap(NULL, guest_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    guest_t g;
    memset(&g, 0, sizeof(g));
    g.host_base = memory;
    g.guest_size = guest_size;
    g.pt_pool_next = guest_page_table_pool_base(&g);

    mem_region_t region = {
        .gpa_start = region_start,
        .gpa_end = region_end,
        .va_base = 0,
        .perms = MEM_PERM_RW,
    };

    int fails = 0;
    if (guest_page_table_pool_base(&g) < region_end) {
        printf("FAIL page-table pool overlaps application mapping arena\n");
        fails++;
    } else {
        printf("OK page-table pool is outside application mapping arena\n");
    }

    if (!guest_build_page_tables(&g, &region, 1)) {
        printf("FAIL initial page tables\n");
        fails++;
    } else {
        int split_count = 0;
        for (int i = 0; i < blocks_to_split; i++) {
            uint64_t block = region_start + (uint64_t)i * BLOCK_2MB;
            if (guest_split_block(&g, block) < 0)
                break;
            split_count++;
        }
        if (split_count != blocks_to_split) {
            printf("FAIL split capacity %d/%d blocks\n", split_count, blocks_to_split);
            fails++;
        } else {
            printf("OK split capacity %d blocks\n", split_count);
        }
    }

    munmap(memory, guest_size);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
