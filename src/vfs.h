/* vfs.h — Deterministic guest VFS: mounts, virtual CWD, path resolver
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modes:
 *   legacy — prior sysroot + host path behavior (CLI default)
 *   rooted — longest-prefix binds, virtual CWD, no host chdir
 */
#ifndef HL_VFS_H
#define HL_VFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HL_VFS_MAX_MOUNTS 64
#define HL_VFS_PATH_MAX   4096
#define HL_VFS_MAX_SYMLINKS 40

typedef enum hl_fs_mode {
    HL_FS_LEGACY = 0,
    HL_FS_ROOTED = 1,
} hl_fs_mode_t;

typedef enum hl_mount_flags {
    HL_MOUNT_RO       = 1u << 0,
    HL_MOUNT_NOEXEC   = 1u << 1,
    HL_MOUNT_NOSUID   = 1u << 2,
    HL_MOUNT_VIRTUAL  = 1u << 3, /* /dev, /proc synthetic */
} hl_mount_flags_t;

typedef struct hl_mount {
    uint64_t id;
    char guest_prefix[HL_VFS_PATH_MAX]; /* absolute guest path, no trailing / except root */
    char host_path[HL_VFS_PATH_MAX];    /* absolute host path (empty if virtual) */
    uint32_t flags;
    size_t guest_len; /* strlen(guest_prefix) for longest-prefix */
} hl_mount_t;

typedef struct hl_vfs {
    hl_fs_mode_t mode;
    hl_mount_t mounts[HL_VFS_MAX_MOUNTS];
    int nmounts;
    char cwd[HL_VFS_PATH_MAX];      /* guest absolute CWD */
    char guest_home[HL_VFS_PATH_MAX];
    char sysroot[HL_VFS_PATH_MAX];  /* loader sysroot (separate from binds) */
    uint64_t next_mount_id;
} hl_vfs_t;

typedef struct hl_vfs_resolve {
    char guest_abs[HL_VFS_PATH_MAX];
    char host_path[HL_VFS_PATH_MAX]; /* final host path when non-virtual */
    char host_parent[HL_VFS_PATH_MAX];
    char leaf[256];
    const hl_mount_t *mount;
    int is_virtual;
    int phase; /* for tracing */
} hl_vfs_resolve_t;

/* Global process VFS (reconstructed on fork). */
hl_vfs_t *hl_vfs_get(void);
void hl_vfs_init_defaults(void);
void hl_vfs_reset(void);

hl_fs_mode_t hl_vfs_mode(void);
void hl_vfs_set_mode(hl_fs_mode_t mode);

int hl_vfs_set_sysroot(const char *path);
const char *hl_vfs_sysroot(void);

int hl_vfs_set_guest_home(const char *guest_path);
int hl_vfs_set_cwd(const char *guest_abs);
const char *hl_vfs_cwd(void);
/* Thread-safe snapshot of the virtual CWD. */
void hl_vfs_cwd_copy(char *out, size_t out_sz);

/* Add bind: guest_prefix -> host_path. Longest prefix wins. */
int hl_vfs_add_bind(const char *guest_prefix, const char *host_path,
                    uint32_t flags);

/* Parse --bind GUEST:HOST or HOST:GUEST? Spec: guest=host form "host:guest"
 * or "guest=host". We accept "HOST:GUEST" as macOS path:guest and
 * "GUEST=HOST". Returns 0 on success. */
int hl_vfs_parse_bind_arg(const char *arg);

/* Component resolver. dirfd_guest: LINUX_AT_FDCWD or guest dir fd.
 * For O_CREAT, final component need not exist (parent+leaf). */
int hl_vfs_resolve_at(int dirfd_guest, const char *guest_path,
                      int follow_final_symlink, int create_mode,
                      hl_vfs_resolve_t *out);

/* Reverse map host absolute path -> guest path. 0 on success. */
int hl_vfs_host_to_guest(const char *host_path, char *guest_out, size_t out_sz);

/* Virtual chdir: never calls host chdir. */
int hl_vfs_chdir(const char *guest_path);
int hl_vfs_fchdir(int guest_fd);
int hl_vfs_getcwd(char *buf, size_t sz);

/* Apply rooted app default profile (home, Music, Desktop, ...). */
int hl_vfs_apply_app_profile(const char *host_home);

/* Fork: deep-copy mount table + cwd into child struct. */
void hl_vfs_fork_export(hl_vfs_t *dst);
void hl_vfs_fork_import(const hl_vfs_t *src);

/* Parse --fs-mode=legacy|rooted */
int hl_vfs_parse_mode(const char *s, hl_fs_mode_t *out);

#endif /* HL_VFS_H */
