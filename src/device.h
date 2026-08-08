/* device.h — Virtual device registry for /dev nodes
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef HL_DEVICE_H
#define HL_DEVICE_H

#include <stdint.h>
#include <stddef.h>
#include "fd_object.h"

typedef struct guest guest_t;

typedef struct hl_device_ops {
    /* Open device; returns guest fd or negative Linux errno. */
    int64_t (*open)(const char *name, int linux_flags, int mode);
    int (*stat)(const char *name, uint32_t *mode_out, uint64_t *rdev_out);
    int (*access)(const char *name, int mode);
} hl_device_ops_t;

typedef struct hl_device_node {
    const char *name; /* basename under /dev, e.g. "dsp" */
    const hl_device_ops_t *ops;
    uint32_t mode;    /* S_IFCHR | perms */
    uint32_t major;
    uint32_t minor;
} hl_device_node_t;

void hl_device_init(void);
int hl_device_register(const hl_device_node_t *node);

/* Path is full guest path "/dev/..." or basename. Returns 1 if handled. */
int hl_device_is_virtual(const char *path);
const hl_device_node_t *hl_device_lookup(const char *path);

/* Open/stat intercepts for /dev. Return -2 if not a virtual device. */
int64_t hl_device_open(const char *path, int linux_flags, int mode);
int hl_device_stat(const char *path, uint32_t *mode_out, uint64_t *rdev_out);

/* Readdir helper: fill next name; index starts at 0. Returns 1 if ok, 0 end. */
int hl_device_readdir(int index, char *name_out, size_t name_sz,
                      uint8_t *dtype_out);

#endif /* HL_DEVICE_H */
