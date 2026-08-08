/* device.c — Virtual device registry
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "device.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "trace.h"
#include "audio_oss.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

#define HL_DEVICE_MAX 64

static hl_device_node_t g_nodes[HL_DEVICE_MAX];
static int g_nnodes = 0;
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inited = 0;

/* Pseudo device open: host /dev/null etc. */
static int64_t open_host_dev(const char *host_path, int linux_flags) {
    int flags = translate_open_flags(linux_flags);
    int hfd = open(host_path, flags, 0);
    if (hfd < 0) return linux_errno();
    int gfd = fd_alloc(FD_REGULAR, hfd);
    if (gfd < 0) {
        close(hfd);
        return -LINUX_EMFILE;
    }
    fd_table[gfd].linux_flags = linux_flags;
    return gfd;
}

static int64_t dev_null_open(const char *name, int linux_flags, int mode) {
    (void)name; (void)mode;
    return open_host_dev("/dev/null", linux_flags);
}
static int64_t dev_zero_open(const char *name, int linux_flags, int mode) {
    (void)name; (void)mode;
    return open_host_dev("/dev/zero", linux_flags);
}
static int64_t dev_urandom_open(const char *name, int linux_flags, int mode) {
    (void)name; (void)mode;
    return open_host_dev("/dev/urandom", linux_flags);
}
static int64_t dev_tty_open(const char *name, int linux_flags, int mode) {
    (void)name; (void)mode;
    return open_host_dev("/dev/tty", linux_flags);
}

/* No .stat override: hl_device_stat() already fills mode and the device
 * numbers from the registry entry. The old dev_chr_stat() zeroed rdev,
 * throwing away the major/minor each node declares. */
static const hl_device_ops_t ops_null = { .open = dev_null_open };
static const hl_device_ops_t ops_zero = { .open = dev_zero_open };
static const hl_device_ops_t ops_urandom = { .open = dev_urandom_open };
static const hl_device_ops_t ops_tty = { .open = dev_tty_open };

void hl_device_init(void) {
    pthread_mutex_lock(&dev_lock);
    if (g_inited) {
        pthread_mutex_unlock(&dev_lock);
        return;
    }
    g_nnodes = 0;
    g_inited = 1;
    pthread_mutex_unlock(&dev_lock);

    static const hl_device_node_t builtins[] = {
        { "null",    &ops_null,    S_IFCHR | 0666, 1, 3 },
        { "zero",    &ops_zero,    S_IFCHR | 0666, 1, 5 },
        { "urandom", &ops_urandom, S_IFCHR | 0666, 1, 9 },
        { "random",  &ops_urandom, S_IFCHR | 0666, 1, 8 },
        { "tty",     &ops_tty,     S_IFCHR | 0666, 5, 0 },
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        hl_device_register(&builtins[i]);

    /* OSS nodes registered from audio_oss_init */
    hl_audio_oss_register_devices();
}

int hl_device_register(const hl_device_node_t *node) {
    if (!node || !node->name) return -1;
    pthread_mutex_lock(&dev_lock);
    if (g_nnodes >= HL_DEVICE_MAX) {
        pthread_mutex_unlock(&dev_lock);
        return -1;
    }
    g_nodes[g_nnodes++] = *node;
    pthread_mutex_unlock(&dev_lock);
    if (hl_trace_on(HL_TRACE_DEV))
        hl_trace(HL_TRACE_DEV, "register /dev/%s major=%u minor=%u",
                 node->name, node->major, node->minor);
    return 0;
}

static const char *basename_dev(const char *path) {
    if (!path) return NULL;
    if (strncmp(path, "/dev/", 5) == 0) return path + 5;
    if (strcmp(path, "/dev") == 0) return "";
    /* Only absolute /dev/ paths name a virtual device. Matching bare
     * single-component names here would hijack ordinary relative files:
     * open("random") in a data directory returned /dev/urandom, and
     * `echo x > null` silently discarded the write. */
    return NULL;
}

const hl_device_node_t *hl_device_lookup(const char *path) {
    const char *base = basename_dev(path);
    if (!base || !*base) return NULL;
    /* strip nested */
    if (strchr(base, '/')) return NULL;
    pthread_mutex_lock(&dev_lock);
    for (int i = 0; i < g_nnodes; i++) {
        if (strcmp(g_nodes[i].name, base) == 0) {
            const hl_device_node_t *n = &g_nodes[i];
            pthread_mutex_unlock(&dev_lock);
            return n;
        }
    }
    pthread_mutex_unlock(&dev_lock);
    return NULL;
}

int hl_device_is_virtual(const char *path) {
    return hl_device_lookup(path) != NULL;
}

int64_t hl_device_open(const char *path, int linux_flags, int mode) {
    const hl_device_node_t *n = hl_device_lookup(path);
    if (!n) return -2; /* not handled */
    if (!n->ops || !n->ops->open) return -LINUX_ENODEV;
    if (hl_trace_on(HL_TRACE_DEV))
        hl_trace(HL_TRACE_DEV, "open %s flags=0x%x", path, linux_flags);
    return n->ops->open(n->name, linux_flags, mode);
}

int hl_device_stat(const char *path, uint32_t *mode_out, uint64_t *rdev_out) {
    const hl_device_node_t *n = hl_device_lookup(path);
    if (!n) return -2;
    if (mode_out) *mode_out = n->mode ? n->mode : (S_IFCHR | 0666);
    if (rdev_out) {
        /* HOST (macOS) encoding: callers stuff this into a struct stat that
         * then goes through translate_stat(), which applies the Linux
         * new_encode_dev() itself. Emitting a Linux-encoded value here meant
         * it was encoded twice — stat("/dev/dsp") reported major 0 minor
         * 1027 while fstat() on the same fd reported something else again. */
        *rdev_out = (uint64_t)makedev(n->major, n->minor);
    }
    if (n->ops && n->ops->stat)
        return n->ops->stat(n->name, mode_out, rdev_out);
    return 0;
}

int hl_device_readdir(int index, char *name_out, size_t name_sz,
                      uint8_t *dtype_out) {
    pthread_mutex_lock(&dev_lock);
    if (index < 0 || index >= g_nnodes) {
        pthread_mutex_unlock(&dev_lock);
        return 0;
    }
    snprintf(name_out, name_sz, "%s", g_nodes[index].name);
    if (dtype_out) *dtype_out = 2; /* DT_CHR */
    pthread_mutex_unlock(&dev_lock);
    return 1;
}
