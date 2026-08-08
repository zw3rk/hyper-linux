/* fd_object.c — Ref-counted open-file descriptions and FD ops core
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fd_object.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>

/* Bitmap + fd_table live in syscall.c; we only use public helpers + fd_lock. */

static atomic_ullong g_object_id = 1;

uint64_t hl_fd_next_object_id(void) {
    return atomic_fetch_add(&g_object_id, 1);
}

/* ---------- Default host file ops (most callbacks NULL → generic path) ---- */

static void host_file_destroy(hl_open_file_t *of) {
    free(of->guest_path_hint);
    of->guest_path_hint = NULL;
    free(of->state);
    of->state = NULL;
}

const hl_fd_ops_t hl_fd_ops_host_file = {
    .destroy = host_file_destroy,
};

const hl_fd_ops_t hl_fd_ops_host_dir = {
    .destroy = host_file_destroy,
};

/* ---------- Open file ---------- */

hl_open_file_t *hl_open_file_create(hl_fd_kind_t kind,
                                    const hl_fd_ops_t *ops,
                                    uint32_t status_flags,
                                    void *state) {
    hl_open_file_t *of = calloc(1, sizeof(*of));
    if (!of) return NULL;
    atomic_store(&of->refcount, 1);
    of->object_id = hl_fd_next_object_id();
    of->kind = kind;
    of->ops = ops;
    atomic_store(&of->status_flags, status_flags);
    of->state = state;
    return of;
}

void hl_open_file_retain(hl_open_file_t *of) {
    if (!of) return;
    atomic_fetch_add(&of->refcount, 1);
}

void hl_open_file_release(hl_open_file_t *of) {
    if (!of) return;
    unsigned prev = atomic_fetch_sub(&of->refcount, 1);
    if (prev == 1) {
        if (of->ops && of->ops->destroy)
            of->ops->destroy(of);
        free(of->guest_path_hint);
        free(of);
    }
}

/* ---------- Descriptor ---------- */

hl_descriptor_t *hl_descriptor_create(hl_open_file_t *of, int host_fd,
                                      uint32_t fd_flags, int type, void *dir) {
    hl_descriptor_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    atomic_store(&d->refcount, 1); /* table ownership */
    d->of = of; /* takes the caller's of ref */
    d->host_fd = host_fd;
    atomic_store(&d->fd_flags, fd_flags);
    atomic_store(&d->removed, false);
    d->dir = dir;
    d->type = type;
    return d;
}

void hl_descriptor_retain(hl_descriptor_t *d) {
    if (!d) return;
    atomic_fetch_add(&d->refcount, 1);
}

void hl_descriptor_release(hl_descriptor_t *d) {
    if (!d) return;
    unsigned prev = atomic_fetch_sub(&d->refcount, 1);
    if (prev == 1) {
        /* Final descriptor ref: close host alias (except stdio). */
        if (d->host_fd >= 0 && d->type != FD_STDIO) {
            close(d->host_fd);
            d->host_fd = -1;
        }
        if (d->dir) {
            if (d->type == FD_DIR)
                closedir((DIR *)d->dir);
            else if (d->type == FD_EPOLL)
                free(d->dir);
            d->dir = NULL;
        }
        hl_open_file_release(d->of);
        d->of = NULL;
        free(d);
    }
}

/* ---------- Sync legacy fd_entry_t fields ---------- */

void hl_fd_sync_legacy(int guest_fd) {
    if (guest_fd < 0 || guest_fd >= FD_TABLE_SIZE) return;
    hl_descriptor_t *d = fd_table[guest_fd].desc;
    if (!d) {
        fd_table[guest_fd].type = FD_CLOSED;
        fd_table[guest_fd].host_fd = -1;
        fd_table[guest_fd].linux_flags = 0;
        fd_table[guest_fd].dir = NULL;
        fd_table[guest_fd].of = NULL;
        return;
    }
    fd_table[guest_fd].type = d->type;
    fd_table[guest_fd].host_fd = d->host_fd;
    fd_table[guest_fd].linux_flags = (int)atomic_load(&d->fd_flags);
    /* Merge status CLOEXEC-ish: linux_flags historically held open flags.
     * Keep CLOEXEC in linux_flags for existing exec sweep. */
    if (d->of) {
        uint32_t st = atomic_load(&d->of->status_flags);
        fd_table[guest_fd].linux_flags |= (int)(st & ~LINUX_O_CLOEXEC);
        if (atomic_load(&d->fd_flags) & LINUX_O_CLOEXEC)
            fd_table[guest_fd].linux_flags |= LINUX_O_CLOEXEC;
    }
    fd_table[guest_fd].dir = d->dir;
    fd_table[guest_fd].of = d->of;
}

/* ---------- Table ops ---------- */

int hl_fd_get(int guest_fd, hl_fd_ref_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (guest_fd < 0 || guest_fd >= FD_TABLE_SIZE) return -1;

    pthread_mutex_lock(&fd_lock);
    if (fd_table[guest_fd].type == FD_CLOSED) {
        pthread_mutex_unlock(&fd_lock);
        return -1;
    }
    hl_descriptor_t *d = fd_table[guest_fd].desc;
    if (d) {
        if (atomic_load(&d->removed)) {
            pthread_mutex_unlock(&fd_lock);
            return -1;
        }
        hl_descriptor_retain(d);
        if (d->of) hl_open_file_retain(d->of);
        out->desc = d;
        out->of = d->of;
        out->host_fd = d->host_fd;
        out->guest_fd = guest_fd;
        out->fd_flags = atomic_load(&d->fd_flags);
        pthread_mutex_unlock(&fd_lock);
        return 0;
    }
    /* Legacy entry without descriptor: synthesize transient view.
     * Retain nothing; host_fd may race — callers on legacy path use
     * fd_to_host as before. For safety, still fill type-compatible ref. */
    out->desc = NULL;
    out->of = fd_table[guest_fd].of;
    if (out->of) hl_open_file_retain(out->of);
    out->host_fd = fd_table[guest_fd].host_fd;
    out->guest_fd = guest_fd;
    out->fd_flags = (uint32_t)(fd_table[guest_fd].linux_flags & LINUX_O_CLOEXEC);
    pthread_mutex_unlock(&fd_lock);
    return 0;
}

void hl_fd_put(hl_fd_ref_t *ref) {
    if (!ref) return;
    if (ref->of) {
        hl_open_file_release(ref->of);
        ref->of = NULL;
    }
    if (ref->desc) {
        hl_descriptor_release(ref->desc);
        ref->desc = NULL;
    }
    ref->host_fd = -1;
    ref->guest_fd = -1;
}

int hl_fd_install(hl_open_file_t *of, int host_fd, uint32_t fd_flags) {
    if (!of) return -1;
    int type = of->kind;
    int gfd = fd_alloc(type, host_fd);
    if (gfd < 0) {
        hl_open_file_release(of);
        if (host_fd >= 0 && type != FD_STDIO) close(host_fd);
        return -1;
    }
    hl_descriptor_t *d = hl_descriptor_create(of, host_fd, fd_flags, type, NULL);
    if (!d) {
        /* of already owned by failed path — release via close path */
        fd_mark_closed(gfd);
        hl_open_file_release(of);
        if (host_fd >= 0 && type != FD_STDIO) close(host_fd);
        return -1;
    }
    pthread_mutex_lock(&fd_lock);
    fd_table[gfd].desc = d;
    fd_table[gfd].of = of;
    hl_fd_sync_legacy(gfd);
    pthread_mutex_unlock(&fd_lock);

    if (hl_trace_on(HL_TRACE_FD))
        hl_trace(HL_TRACE_FD, "install gfd=%d kind=%d host_fd=%d of=%llu",
                 gfd, type, host_fd, (unsigned long long)of->object_id);
    return gfd;
}

int hl_fd_install_at(int guest_fd, hl_open_file_t *of,
                     int host_fd, uint32_t fd_flags, bool replace) {
    if (!of || guest_fd < 0 || guest_fd >= FD_TABLE_SIZE) {
        if (of) hl_open_file_release(of);
        return -1;
    }
    int type = of->kind;
    hl_descriptor_t *d = hl_descriptor_create(of, host_fd, fd_flags, type, NULL);
    if (!d) {
        hl_open_file_release(of);
        return -1;
    }

    /* Close any existing occupant first (outside install lock pattern). */
    if (replace) {
        hl_fd_detached_t old = {0};
        if (hl_fd_remove(guest_fd, &old) == 0)
            hl_fd_detached_finish(&old);
    } else if (fd_table[guest_fd].type != FD_CLOSED) {
        hl_descriptor_release(d);
        return -1;
    }

    if (fd_alloc_at(guest_fd, type, host_fd) < 0) {
        hl_descriptor_release(d);
        return -1;
    }
    pthread_mutex_lock(&fd_lock);
    fd_table[guest_fd].desc = d;
    fd_table[guest_fd].of = of;
    hl_fd_sync_legacy(guest_fd);
    pthread_mutex_unlock(&fd_lock);
    return guest_fd;
}

int hl_fd_remove(int guest_fd, hl_fd_detached_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (guest_fd < 0 || guest_fd >= FD_TABLE_SIZE) return -1;

    pthread_mutex_lock(&fd_lock);
    if (fd_table[guest_fd].type == FD_CLOSED) {
        pthread_mutex_unlock(&fd_lock);
        return -1;
    }
    out->desc = fd_table[guest_fd].desc;
    out->type = fd_table[guest_fd].type;
    out->host_fd = fd_table[guest_fd].host_fd;
    out->dir = fd_table[guest_fd].dir;
    out->of = fd_table[guest_fd].of;
    if (out->desc)
        atomic_store(&out->desc->removed, true);

    fd_table[guest_fd].type = FD_CLOSED;
    fd_table[guest_fd].host_fd = -1;
    fd_table[guest_fd].dir = NULL;
    fd_table[guest_fd].linux_flags = 0;
    fd_table[guest_fd].desc = NULL;
    fd_table[guest_fd].of = NULL;
    fd_mark_closed_unlocked(guest_fd);
    pthread_mutex_unlock(&fd_lock);
    return 0;
}

void hl_fd_detached_finish(hl_fd_detached_t *d) {
    if (!d) return;
    if (d->desc) {
        /* Descriptor release closes host_fd and dir and open-file. */
        hl_descriptor_release(d->desc);
        d->desc = NULL;
        d->of = NULL;
        d->dir = NULL;
        d->host_fd = -1;
        return;
    }
    /* Legacy path without descriptor object */
    if (d->dir) {
        if (d->type == FD_DIR) closedir((DIR *)d->dir);
        else if (d->type == FD_EPOLL || d->type == FD_VIRTUAL_DIR) free(d->dir);
        d->dir = NULL;
    }
    if (d->of) {
        hl_open_file_release(d->of);
        d->of = NULL;
    }
    if (d->host_fd >= 0 && d->type != FD_STDIO) {
        close(d->host_fd);
        d->host_fd = -1;
    }
}

int hl_fd_dup(int oldfd) {
    return hl_fd_dup_from(oldfd, 0, 0);
}

/* dup(2) is hl_fd_dup_from(fd, 0, 0). F_DUPFD needs a minimum fd number and
 * F_DUPFD_CLOEXEC needs the flag set — routing both through hl_fd_dup()
 * ignored `arg` entirely and always cleared CLOEXEC. */
int hl_fd_dup_from(int oldfd, int minfd, int cloexec) {
    hl_fd_ref_t ref;
    if (minfd < 0 || minfd >= FD_TABLE_SIZE) return -LINUX_EINVAL;
    if (hl_fd_get(oldfd, &ref) < 0) return -LINUX_EBADF;

    int new_host = -1;
    if (ref.host_fd >= 0) {
        new_host = dup(ref.host_fd);
        if (new_host < 0) {
            hl_fd_put(&ref);
            return linux_errno();
        }
    }

    hl_open_file_t *of = ref.of;
    if (of)
        hl_open_file_retain(of);
    else {
        /* Pure legacy: allocate plain fd entry sharing... can't share offset
         * without of; fall back to independent host dup only. */
        int gfd = fd_alloc_from(minfd, fd_table[oldfd].type, new_host);
        if (gfd < 0) {
            if (new_host >= 0) close(new_host);
            hl_fd_put(&ref);
            return -LINUX_EMFILE;
        }
        if (cloexec && new_host >= 0)
            fcntl(new_host, F_SETFD, FD_CLOEXEC);
        pthread_mutex_lock(&fd_lock);
        fd_table[gfd].linux_flags =
            (fd_table[oldfd].linux_flags & ~LINUX_O_CLOEXEC)
            | (cloexec ? LINUX_O_CLOEXEC : 0);
        /* dir not shared for legacy DIR without of */
        pthread_mutex_unlock(&fd_lock);
        hl_fd_put(&ref);
        return gfd;
    }

    int kind = of->kind;
    uint32_t st = atomic_load(&of->status_flags);
    (void)st;
    uint32_t new_fd_flags = cloexec ? LINUX_O_CLOEXEC : 0;
    if (cloexec && new_host >= 0)
        fcntl(new_host, F_SETFD, FD_CLOEXEC);
    hl_descriptor_t *nd = hl_descriptor_create(of, new_host, new_fd_flags,
                                               kind, NULL);
    if (!nd) {
        if (new_host >= 0) close(new_host);
        hl_open_file_release(of);
        hl_fd_put(&ref);
        return -LINUX_ENOMEM;
    }

    int gfd = fd_alloc_from(minfd, kind, new_host);
    if (gfd < 0) {
        hl_descriptor_release(nd);
        hl_fd_put(&ref);
        return -LINUX_EMFILE;
    }
    pthread_mutex_lock(&fd_lock);
    fd_table[gfd].desc = nd;
    fd_table[gfd].of = of;
    hl_fd_sync_legacy(gfd);
    /* dup(2) clears CLOEXEC; F_DUPFD_CLOEXEC sets it. */
    fd_table[gfd].linux_flags =
        (fd_table[gfd].linux_flags & ~LINUX_O_CLOEXEC) | new_fd_flags;
    atomic_store(&nd->fd_flags, new_fd_flags);
    pthread_mutex_unlock(&fd_lock);

    hl_fd_put(&ref);
    if (hl_trace_on(HL_TRACE_FD))
        hl_trace(HL_TRACE_FD, "dup old=%d new=%d host=%d", oldfd, gfd, new_host);
    return gfd;
}

int hl_fd_dup3(int oldfd, int newfd, int flags) {
    if (oldfd == newfd) {
        /* Linux: dup3(old,old,...) → EINVAL; dup2(old,old) → old */
        return -LINUX_EINVAL;
    }
    if (newfd < 0 || newfd >= FD_TABLE_SIZE) return -LINUX_EBADF;

    hl_fd_ref_t ref;
    if (hl_fd_get(oldfd, &ref) < 0) return -LINUX_EBADF;

    int new_host = -1;
    if (ref.host_fd >= 0) {
        new_host = dup(ref.host_fd);
        if (new_host < 0) {
            hl_fd_put(&ref);
            return linux_errno();
        }
        if (flags & LINUX_O_CLOEXEC)
            fcntl(new_host, F_SETFD, FD_CLOEXEC);
    }

    hl_open_file_t *of = ref.of;
    if (of) hl_open_file_retain(of);

    uint32_t fd_flags = (flags & LINUX_O_CLOEXEC) ? LINUX_O_CLOEXEC : 0;
    int kind = of ? of->kind : fd_table[oldfd].type;
    hl_descriptor_t *nd = NULL;
    if (of) {
        nd = hl_descriptor_create(of, new_host, fd_flags, kind, NULL);
        if (!nd) {
            if (new_host >= 0) close(new_host);
            if (of) hl_open_file_release(of);
            hl_fd_put(&ref);
            return -LINUX_ENOMEM;
        }
    }

    /* Close target and install. The special-FD subsystems key their state
     * on the guest fd number, so they must be told too — otherwise the old
     * eventfd/signalfd/timerfd/inotify state stayed alive under a number
     * now owned by this dup, and the next creator to reuse that number
     * inherited it (a recreated eventfd never became poll-ready). */
    hl_fd_detached_t old = {0};
    int had = (hl_fd_remove(newfd, &old) == 0);
    if (had) {
        fd_special_subsystem_close(newfd, old.type);
        hl_fd_detached_finish(&old);
    }

    if (fd_alloc_at(newfd, kind, new_host) < 0) {
        if (nd) hl_descriptor_release(nd);
        else if (new_host >= 0) close(new_host);
        hl_fd_put(&ref);
        return -LINUX_EBADF;
    }
    pthread_mutex_lock(&fd_lock);
    fd_table[newfd].desc = nd;
    fd_table[newfd].of = of;
    if (nd) hl_fd_sync_legacy(newfd);
    else {
        fd_table[newfd].linux_flags = (int)fd_flags;
    }
    pthread_mutex_unlock(&fd_lock);

    hl_fd_put(&ref);
    return newfd;
}
