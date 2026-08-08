/* fd_object.h — Ref-counted open-file descriptions and FD ops core
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux model:
 *   - descriptor table entry: guest FD number + FD_CLOEXEC (+ host alias)
 *   - open file description: shared by dup; status flags, kind, ops, state
 *
 * Compatibility: existing fd_entry_t fields (type, host_fd, linux_flags, dir)
 * remain authoritative for legacy paths; of points at the shared open-file
 * when present. OSS/virtual devices always use of->ops.
 *
 * Lock order: fd_lock (3) protects the table. Object refcount is atomic;
 * destroy() runs outside fd_lock. Never take mmap_lock while holding a
 * transient fd ref if that path also needs mmap — release first.
 */
#ifndef FD_OBJECT_H
#define FD_OBJECT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

/* Forward-declarable: guest.h uses `typedef struct guest { ... } guest_t`. */
typedef struct guest guest_t;

/* Forward kinds align with syscall.h FD_* constants for compatibility. */
#ifndef FD_CLOSED
#define FD_CLOSED   0
#define FD_STDIO    1
#define FD_REGULAR  2
#define FD_DIR      3
#define FD_PIPE     4
#define FD_SOCKET   5
#define FD_EPOLL    6
#define FD_TIMERFD  7
#define FD_EVENTFD  8
#define FD_SIGNALFD 9
#define FD_INOTIFY  10
#endif

/* New kinds for virtual devices (keep numeric values stable once published). */
#define FD_VIRTUAL_DIR  11
#define FD_OSS_DSP      12
#define FD_OSS_MIXER    13
#define FD_DEVICE       14

typedef int hl_fd_kind_t;

typedef struct hl_open_file hl_open_file_t;
typedef struct hl_fd_ops hl_fd_ops_t;
typedef struct hl_descriptor hl_descriptor_t;

/* Fork serialization records (opaque payloads owned by type ops). */
typedef struct hl_fork_object_record {
    uint32_t kind;
    uint32_t status_flags;
    uint64_t object_id;
    uint64_t mount_id;
    uint32_t payload_len;
    /* payload follows in IPC stream when payload_len > 0 */
    uint8_t payload[256];
} hl_fork_object_record_t;

typedef struct hl_fork_context {
    /* Reserved for import-time host resources (shm map, etc.). */
    int dummy;
} hl_fork_context_t;

struct hl_fd_ops {
    int64_t (*read)(hl_open_file_t *of, int host_fd,
                    guest_t *g, uint64_t buf_gva, uint64_t count);
    int64_t (*write)(hl_open_file_t *of, int host_fd,
                     guest_t *g, uint64_t buf_gva, uint64_t count);
    int64_t (*readv)(hl_open_file_t *of, int host_fd,
                     guest_t *g, uint64_t iov_gva, int iovcnt);
    int64_t (*writev)(hl_open_file_t *of, int host_fd,
                      guest_t *g, uint64_t iov_gva, int iovcnt);
    int64_t (*ioctl)(hl_open_file_t *of, int host_fd,
                     guest_t *g, uint64_t request, uint64_t arg_gva);
    int64_t (*lseek)(hl_open_file_t *of, int host_fd,
                     int64_t offset, int whence);
    int64_t (*fstat)(hl_open_file_t *of, int host_fd,
                     guest_t *g, uint64_t stat_gva);
    int64_t (*fcntl_getfl)(hl_open_file_t *of, int host_fd);
    int64_t (*fcntl_setfl)(hl_open_file_t *of, int host_fd,
                           uint32_t linux_flags);
    /* Host fd used for poll/select/kqueue; -1 if custom. */
    int (*poll_host_fd)(hl_open_file_t *of, int descriptor_host_fd);
    void (*destroy)(hl_open_file_t *of);
    int (*fork_export)(hl_open_file_t *of, hl_fork_object_record_t *out);
    hl_open_file_t *(*fork_import)(const hl_fork_object_record_t *in,
                                   const hl_fork_context_t *ctx);
};

struct hl_open_file {
    atomic_uint refcount;
    uint64_t object_id;
    hl_fd_kind_t kind;
    const hl_fd_ops_t *ops;
    atomic_uint status_flags; /* O_NONBLOCK, O_APPEND, access mode bits */
    void *state;
    uint64_t mount_id;
    char *guest_path_hint;
};

/* Per-descriptor object: table ownership + active syscall refs.
 * Host alias closes only when final descriptor ref is released. */
struct hl_descriptor {
    atomic_uint refcount;
    hl_open_file_t *of;
    int host_fd;
    atomic_uint fd_flags; /* FD_CLOEXEC */
    atomic_bool removed;
    void *dir; /* DIR* or epoll_instance until fully migrated to of->state */
    int type;  /* cached kind for fast legacy reads */
};

/* Transient ref held across a syscall. */
typedef struct hl_fd_ref {
    hl_descriptor_t *desc;
    hl_open_file_t *of;
    int host_fd;
    int guest_fd;
    uint32_t fd_flags;
} hl_fd_ref_t;

typedef struct hl_fd_detached {
    hl_descriptor_t *desc; /* still held; caller must hl_descriptor_release */
    int host_fd;
    int type;
    void *dir;
    hl_open_file_t *of;
} hl_fd_detached_t;

/* ---------- Open-file lifetime ---------- */

hl_open_file_t *hl_open_file_create(hl_fd_kind_t kind,
                                    const hl_fd_ops_t *ops,
                                    uint32_t status_flags,
                                    void *state);
void hl_open_file_retain(hl_open_file_t *of);
void hl_open_file_release(hl_open_file_t *of);

/* ---------- Descriptor lifetime ---------- */

hl_descriptor_t *hl_descriptor_create(hl_open_file_t *of, int host_fd,
                                      uint32_t fd_flags, int type, void *dir);
void hl_descriptor_retain(hl_descriptor_t *d);
void hl_descriptor_release(hl_descriptor_t *d);

/* ---------- Table API ---------- */

/* Lookup and pin descriptor + open-file for syscall duration. */
int hl_fd_get(int guest_fd, hl_fd_ref_t *out);
void hl_fd_put(hl_fd_ref_t *ref);

/* Install: takes ownership of one open-file ref and host_fd. */
int hl_fd_install(hl_open_file_t *of, int host_fd, uint32_t fd_flags);
int hl_fd_install_at(int guest_fd, hl_open_file_t *of,
                     int host_fd, uint32_t fd_flags, bool replace);

/* Remove table entry; cleanup outside lock via detached. */
int hl_fd_remove(int guest_fd, hl_fd_detached_t *out);
void hl_fd_detached_finish(hl_fd_detached_t *d);
/* Release an fd's `.dir` by kind (closedir/free/nothing). The one whitelist
 * every fd-retire path must use — see fd_object.c. */
void hl_fd_free_dir(int type, void *dir);

/* Dup helpers: new descriptor sharing open-file; host dup of alias. */
int hl_fd_dup(int oldfd);
/* dup, but starting the search at minfd and optionally setting CLOEXEC —
 * the semantics F_DUPFD / F_DUPFD_CLOEXEC need. */
int hl_fd_dup_from(int oldfd, int minfd, int cloexec);
int hl_fd_dup3(int oldfd, int newfd, int flags);

/* Ensure legacy fd_entry fields mirror descriptor (after install). */
void hl_fd_sync_legacy(int guest_fd);

/* Next object id (for tracing / fork). */
uint64_t hl_fd_next_object_id(void);

/* Default ops for host-backed regular/stdio/pipe/socket files (NULL ioctl). */
extern const hl_fd_ops_t hl_fd_ops_host_file;
extern const hl_fd_ops_t hl_fd_ops_host_dir;

/* IPC version bump note for fork: when open-file graph is serialized. */
#define HL_FD_FORK_GRAPH_VERSION 1

#endif /* FD_OBJECT_H */
