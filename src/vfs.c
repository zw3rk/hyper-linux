/* vfs.c — Deterministic guest VFS: mounts, virtual CWD, path resolver
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vfs.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "trace.h"
#include "fd_object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <pthread.h>

static hl_vfs_t g_vfs;
static pthread_mutex_t vfs_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_vfs_inited = 0;

hl_vfs_t *hl_vfs_get(void) {
    if (!g_vfs_inited) hl_vfs_init_defaults();
    return &g_vfs;
}

void hl_vfs_init_defaults(void) {
    pthread_mutex_lock(&vfs_lock);
    if (!g_vfs_inited) {
        memset(&g_vfs, 0, sizeof(g_vfs));
        g_vfs.mode = HL_FS_LEGACY;
        snprintf(g_vfs.cwd, sizeof(g_vfs.cwd), "/");
        snprintf(g_vfs.guest_home, sizeof(g_vfs.guest_home), "/root");
        g_vfs.next_mount_id = 1;
        g_vfs_inited = 1;
    }
    pthread_mutex_unlock(&vfs_lock);
}

void hl_vfs_reset(void) {
    pthread_mutex_lock(&vfs_lock);
    memset(&g_vfs, 0, sizeof(g_vfs));
    g_vfs.mode = HL_FS_LEGACY;
    snprintf(g_vfs.cwd, sizeof(g_vfs.cwd), "/");
    g_vfs.next_mount_id = 1;
    g_vfs_inited = 1;
    pthread_mutex_unlock(&vfs_lock);
}

hl_fs_mode_t hl_vfs_mode(void) {
    return hl_vfs_get()->mode;
}

void hl_vfs_set_mode(hl_fs_mode_t mode) {
    hl_vfs_get()->mode = mode;
}

int hl_vfs_parse_mode(const char *s, hl_fs_mode_t *out) {
    if (!s || !out) return -1;
    if (strcmp(s, "legacy") == 0) { *out = HL_FS_LEGACY; return 0; }
    if (strcmp(s, "rooted") == 0) { *out = HL_FS_ROOTED; return 0; }
    return -1;
}

int hl_vfs_set_sysroot(const char *path) {
    hl_vfs_t *v = hl_vfs_get();
    if (!path) {
        v->sysroot[0] = '\0';
        return 0;
    }
    snprintf(v->sysroot, sizeof(v->sysroot), "%s", path);
    return 0;
}

const char *hl_vfs_sysroot(void) {
    hl_vfs_t *v = hl_vfs_get();
    return v->sysroot[0] ? v->sysroot : NULL;
}

int hl_vfs_set_guest_home(const char *guest_path) {
    hl_vfs_t *v = hl_vfs_get();
    if (!guest_path || guest_path[0] != '/') return -1;
    snprintf(v->guest_home, sizeof(v->guest_home), "%s", guest_path);
    return 0;
}

int hl_vfs_set_cwd(const char *guest_abs) {
    if (!guest_abs || guest_abs[0] != '/') return -1;
    hl_vfs_t *v = hl_vfs_get();
    /* chdir() on one guest thread runs concurrently with path resolution on
     * every other one. The in-place snprintf + trailing-slash trim below
     * briefly leaves a torn string, so a concurrent reader could resolve a
     * relative path against a truncated directory — opening or deleting the
     * wrong object. vfs_lock previously guarded only init/reset/fork. */
    pthread_mutex_lock(&vfs_lock);
    snprintf(v->cwd, sizeof(v->cwd), "%s", guest_abs);
    /* Normalize trailing slashes except root */
    size_t n = strlen(v->cwd);
    while (n > 1 && v->cwd[n - 1] == '/') {
        v->cwd[--n] = '\0';
    }
    pthread_mutex_unlock(&vfs_lock);
    return 0;
}

const char *hl_vfs_cwd(void) {
    return hl_vfs_get()->cwd;
}

/* Stable snapshot of the virtual CWD for readers on other threads. */
void hl_vfs_cwd_copy(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    hl_vfs_t *v = hl_vfs_get();
    pthread_mutex_lock(&vfs_lock);
    snprintf(out, out_sz, "%s", v->cwd);
    pthread_mutex_unlock(&vfs_lock);
}

static void strip_trailing_slash(char *s) {
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
        s[--n] = '\0';
}

/* realpath() for a path whose tail may not exist yet.
 *
 * Plain realpath() fails outright on a missing component, and the caller
 * then stored the mount root VERBATIM. On macOS that is usually a symlink
 * (/tmp -> /private/tmp, mktemp's /var/... -> /private/var/...), so the
 * containment check compared a canonical parent against a non-canonical
 * root, never matched, and every access to that mount failed with EACCES —
 * permanently, even once the directory was created. Resolve the deepest
 * existing ancestor and re-attach the missing tail.
 * Returns dst on success, NULL if nothing could be resolved. */
static char *canonicalize_partial(const char *path, char *dst, size_t dstsz) {
    if (!path || !dst || dstsz == 0) return NULL;
    if (realpath(path, dst)) return dst;

    char work[HL_VFS_PATH_MAX];
    if ((size_t)snprintf(work, sizeof(work), "%s", path) >= sizeof(work))
        return NULL;

    /* Strip trailing components until one resolves, remembering the tail. */
    char tail[HL_VFS_PATH_MAX];
    size_t tail_len = 0;
    tail[0] = '\0';
    for (;;) {
        char *slash = strrchr(work, '/');
        if (!slash || slash == work) break;      /* reached "/" */
        size_t seg_len = strlen(slash + 1);
        if (seg_len == 0) { *slash = '\0'; continue; }   /* trailing '/' */
        if (tail_len + seg_len + 2 >= sizeof(tail)) return NULL;
        /* Prepend this segment to the accumulated tail. */
        memmove(tail + seg_len + 1, tail, tail_len + 1);
        memcpy(tail, slash + 1, seg_len);
        tail[seg_len] = tail_len ? '/' : '\0';
        tail_len += seg_len + (tail_len ? 1 : 0);
        *slash = '\0';

        char base[HL_VFS_PATH_MAX];
        if (realpath(work, base)) {
            int n = snprintf(dst, dstsz, "%s%s%s", base,
                             (base[1] == '\0') ? "" : "/", tail);
            return (n > 0 && (size_t)n < dstsz) ? dst : NULL;
        }
    }
    return NULL;
}

int hl_vfs_add_bind(const char *guest_prefix, const char *host_path,
                    uint32_t flags) {
    hl_vfs_t *v = hl_vfs_get();
    if (!guest_prefix || guest_prefix[0] != '/') return -1;
    if (v->nmounts >= HL_VFS_MAX_MOUNTS) return -1;

    hl_mount_t *m = &v->mounts[v->nmounts];
    memset(m, 0, sizeof(*m));
    m->id = v->next_mount_id++;
    snprintf(m->guest_prefix, sizeof(m->guest_prefix), "%s", guest_prefix);
    strip_trailing_slash(m->guest_prefix);
    if (host_path) {
        /* Canonicalize the mount root. macOS hands out symlinked paths for
         * common roots (/tmp -> /private/tmp, mktemp's /var/... ->
         * /private/var/...), and fcntl(F_GETPATH) always reports the
         * resolved form. Storing the resolved form keeps forward
         * resolution, the reverse map and dirfd lookups consistent, and
         * stops a bind root from being mistaken for a symlink to re-map.
         * If the path does not exist yet, keep it verbatim. */
        char real[HL_VFS_PATH_MAX];
        if (!(flags & HL_MOUNT_VIRTUAL) &&
            canonicalize_partial(host_path, real, sizeof(real)))
            snprintf(m->host_path, sizeof(m->host_path), "%s", real);
        else
            snprintf(m->host_path, sizeof(m->host_path), "%s", host_path);
        strip_trailing_slash(m->host_path);
    }
    m->flags = flags;
    m->guest_len = strlen(m->guest_prefix);
    v->nmounts++;

    if (hl_trace_on(HL_TRACE_FS))
        hl_trace(HL_TRACE_FS, "bind guest=%s host=%s flags=0x%x id=%llu",
                 m->guest_prefix, m->host_path, flags,
                 (unsigned long long)m->id);
    return 0;
}

/* Accept "HOST:GUEST" (host path may contain ':') by splitting on the last
 * ':' that introduces an absolute guest path, or the legacy "GUEST=HOST".
 * An optional ":ro" suffix makes the mount read-only.
 *
 * The HOST:GUEST form is tried FIRST. Testing '=' first meant a host path
 * containing '=' (legal on macOS) silently took the other branch — with the
 * operands in the opposite order — so `--bind /data=v2:/data` bound host
 * "v2:/data" onto guest "/data=v2" and exited 0 with no diagnostic. */
int hl_vfs_parse_bind_arg(const char *arg) {
    if (!arg || !*arg) return -1;

    char spec[HL_VFS_PATH_MAX * 2];
    if (snprintf(spec, sizeof(spec), "%s", arg) >= (int)sizeof(spec))
        return -1;   /* too long: refuse rather than bind a truncated path */

    uint32_t flags = 0;
    size_t sl = strlen(spec);
    if (sl > 3 && strcmp(spec + sl - 3, ":ro") == 0) {
        flags |= HL_MOUNT_RO;
        spec[sl - 3] = '\0';
    }

    char host[HL_VFS_PATH_MAX], guest[HL_VFS_PATH_MAX];

    /* HOST:GUEST — last colon whose right-hand side is an absolute path. */
    const char *colon = strrchr(spec, ':');
    if (colon && colon != spec && colon[1] == '/') {
        size_t hlen = (size_t)(colon - spec);
        if (hlen >= sizeof(host)) return -1;
        memcpy(host, spec, hlen);
        host[hlen] = '\0';
        if (snprintf(guest, sizeof(guest), "%s", colon + 1) >= (int)sizeof(guest))
            return -1;
        return hl_vfs_add_bind(guest, host, flags);
    }

    /* GUEST=HOST */
    const char *eq = strchr(spec, '=');
    if (eq && eq != spec) {
        size_t glen = (size_t)(eq - spec);
        if (glen >= sizeof(guest)) return -1;
        memcpy(guest, spec, glen);
        guest[glen] = '\0';
        if (snprintf(host, sizeof(host), "%s", eq + 1) >= (int)sizeof(host))
            return -1;
        return hl_vfs_add_bind(guest, host, flags);
    }

    return -1;
}

/* Longest-prefix mount lookup. */
static const hl_mount_t *find_mount(const hl_vfs_t *v, const char *guest_abs) {
    const hl_mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < v->nmounts; i++) {
        const hl_mount_t *m = &v->mounts[i];
        size_t len = m->guest_len;
        if (len == 0) continue;
        if (strncmp(guest_abs, m->guest_prefix, len) != 0) continue;
        /* Boundary: exact or next char is '/' (or prefix is "/") */
        if (guest_abs[len] != '\0' && guest_abs[len] != '/' &&
            !(len == 1 && m->guest_prefix[0] == '/'))
            continue;
        if (len > best_len) {
            best = m;
            best_len = len;
        }
    }
    return best;
}

static int join_path(char *dst, size_t dstsz, const char *a, const char *b) {
    if (!b || !b[0]) {
        snprintf(dst, dstsz, "%s", a);
        return 0;
    }
    if (b[0] == '/') {
        snprintf(dst, dstsz, "%s", b);
        return 0;
    }
    size_t al = strlen(a);
    if (al == 1 && a[0] == '/')
        snprintf(dst, dstsz, "/%s", b);
    else
        snprintf(dst, dstsz, "%s/%s", a, b);
    return 0;
}

/* Normalize guest path: resolve . and .. within the string; no host I/O. */
static int normalize_guest(char *path) {
    char out[HL_VFS_PATH_MAX];
    char *components[256];
    int n = 0;
    if (path[0] != '/') return -1;

    char tmp[HL_VFS_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) {
            if (n > 0) n--;
            continue;
        }
        /* Silently dropping components past the cap while ".." still
         * popped made a long path resolve to a DIFFERENT existing path
         * instead of failing — destructive for unlink/rename/O_TRUNC. */
        if (n >= 256) return -1;
        components[n++] = tok;
    }
    out[0] = '/';
    out[1] = '\0';
    for (int i = 0; i < n; i++) {
        if (i == 0)
            snprintf(out, sizeof(out), "/%s", components[i]);
        else {
            size_t cur = strlen(out);
            snprintf(out + cur, sizeof(out) - cur, "/%s", components[i]);
        }
    }
    if (n == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    snprintf(path, HL_VFS_PATH_MAX, "%s", out);
    return 0;
}

/* Read-only synthetic directory: "/" and the interior path components that
 * lead to a mount point. HL_MOUNT_VIRTUAL keeps callers from treating the
 * guest path as a host path for mutation ops. */
static const hl_mount_t vfs_synthetic_dir_mount = {
    .id = 0,
    .guest_prefix = "/",
    .host_path = "",
    .flags = HL_MOUNT_VIRTUAL | HL_MOUNT_RO,
    .guest_len = 1,
};

/* True when the canonicalized host_dir lies inside mount_root.
 *
 * Used to catch interior-symlink escapes: the resolver builds host paths by
 * string concatenation, so a symlink in any non-final component is followed
 * by the host kernel and can leave the bind. Both sides are canonical
 * (hl_vfs_add_bind() realpath()s mount roots), so a prefix test on a
 * component boundary is sufficient.
 *
 * A path that does not resolve yet (ENOENT on a component) has not been
 * subverted — the caller's own syscall will fail naturally — so that case is
 * allowed through rather than reported as an escape. */
static int real_within(const char *path, const char *mount_root, size_t rl) {
    char real[HL_VFS_PATH_MAX];
    if (!realpath(path, real)) return -1;           /* does not resolve */
    if (strncmp(real, mount_root, rl) != 0) return 0;
    /* Exact match, or a proper component boundary — "/foo" must not match
     * "/foobar". A root mount ("/") contains everything. */
    return (real[rl] == '\0' || real[rl] == '/' || rl == 1) ? 1 : 0;
}

static int path_within_mount(const char *host_path, const char *host_parent,
                             const char *mount_root) {
    if (!mount_root || !mount_root[0]) return 1;   /* virtual: no host root */

    size_t rl = strlen(mount_root);
    while (rl > 1 && mount_root[rl - 1] == '/') rl--;

    /* The mount root itself is inside the mount by definition, and its
     * parent legitimately is not — check the full path first, but only to
     * ACCEPT. */
    int r = real_within(host_path, mount_root, rl);
    if (r == 1) return 1;

    /* Otherwise judge by the parent directory alone.
     *
     * Using the full path to REJECT was too strict: it canonicalizes the
     * final component too, so a symlink pointing outside the bind was
     * refused with EACCES even for operations that never touch its target
     * — readlink, lstat, unlink, rename, symlink, open(O_NOFOLLOW). Those
     * are exactly what the resolver's own S_ISLNK branch below exists to
     * serve, and it never got the chance. When the final symlink IS to be
     * followed, that branch rewrites guest_abs to the target and re-enters
     * this resolver, so the target is checked against the mount table on
     * its own terms rather than escaping.
     *
     * A parent that does not resolve has not been subverted (the caller's
     * own syscall will fail naturally), so that is allowed through. */
    r = real_within(host_parent, mount_root, rl);
    return (r < 0) ? 1 : r;
}

static int mount_ancestor(const hl_vfs_t *v, const char *guest_abs);

/* List the immediate children of a synthetic ancestor directory — the
 * distinct next path components of every mount below guest_abs. This is
 * what makes `ls /` work in rooted mode: "/" and the interior directories
 * of a mount path exist only in the mount table, so there is no host
 * directory to read. Returns the number of names written. */
int hl_vfs_is_synthetic_dir(const char *guest_abs) {
    if (!guest_abs) return 0;
    return mount_ancestor(hl_vfs_get(), guest_abs);
}

int hl_vfs_list_synthetic(const char *guest_abs, char (*names)[HL_VFS_NAME_MAX],
                          int max) {
    if (!guest_abs || !names || max <= 0) return 0;
    hl_vfs_t *v = hl_vfs_get();
    size_t plen = strlen(guest_abs);
    int is_root = (plen == 1 && guest_abs[0] == '/');
    if (!is_root) while (plen > 1 && guest_abs[plen - 1] == '/') plen--;

    int n = 0;
    for (int i = 0; i < v->nmounts && n < max; i++) {
        const char *gp = v->mounts[i].guest_prefix;
        if (!gp || gp[0] != '/') continue;
        const char *rest;
        if (is_root) {
            rest = gp + 1;
        } else {
            if (strncmp(gp, guest_abs, plen) != 0 || gp[plen] != '/') continue;
            rest = gp + plen + 1;
        }
        if (!*rest) continue;                    /* the mount itself */
        const char *slash = strchr(rest, '/');
        size_t clen = slash ? (size_t)(slash - rest) : strlen(rest);
        if (clen == 0 || clen >= HL_VFS_NAME_MAX) continue;

        int dup = 0;
        for (int k = 0; k < n; k++)
            if (strlen(names[k]) == clen && strncmp(names[k], rest, clen) == 0) {
                dup = 1; break;
            }
        if (dup) continue;
        memcpy(names[n], rest, clen);
        names[n][clen] = '\0';
        n++;
    }
    return n;
}

/* True when guest_abs is "/" or a proper ancestor directory of some mount
 * point, e.g. "/home" for a mount at "/home/user". */
static int mount_ancestor(const hl_vfs_t *v, const char *guest_abs) {
    if (guest_abs[0] == '/' && guest_abs[1] == '\0') return 1;   /* "/" */
    size_t len = strlen(guest_abs);
    for (int i = 0; i < v->nmounts; i++) {
        const char *gp = v->mounts[i].guest_prefix;
        if (strncmp(gp, guest_abs, len) == 0 && gp[len] == '/')
            return 1;
    }
    return 0;
}

/* Map a guest directory fd to its guest absolute path.
 *
 * Prefer an explicit hint on the open-file object when one is set. Plain
 * directory opens go through fd_alloc and have no open-file object at all,
 * so fall back to asking the host for the descriptor's current path and
 * running that back through the mount table. Without this fallback every
 * *at() call with a real dirfd failed with ENOTDIR in rooted mode, because
 * guest_path_hint is never assigned anywhere. */
static int dirfd_guest_path(int dirfd_guest, char *out, size_t out_sz) {
    if (dirfd_guest < 0 || dirfd_guest >= FD_TABLE_SIZE ||
        fd_table[dirfd_guest].type == FD_CLOSED)
        return -LINUX_EBADF;

    if (fd_table[dirfd_guest].of && fd_table[dirfd_guest].of->guest_path_hint) {
        snprintf(out, out_sz, "%s", fd_table[dirfd_guest].of->guest_path_hint);
        return 0;
    }

    /* A synthetic directory fd (rooted "/" and mount-ancestor dirs) has no
     * host fd; its guest path lives on the hl_vdir_t so it can act as a
     * dirfd for openat/fstatat (V17: find/, ls -R). */
    if (fd_table[dirfd_guest].type == FD_VIRTUAL_DIR) {
        const char *gp = hl_vdir_path(fd_table[dirfd_guest].dir);
        if (!gp) return -LINUX_ENOTDIR;
        snprintf(out, out_sz, "%s", gp);
        return 0;
    }

    int host_fd = fd_to_host(dirfd_guest);
    if (host_fd < 0) return -LINUX_ENOTDIR;

    /* F_GETPATH needs a buffer of at least MAXPATHLEN. */
    char hostbuf[HL_VFS_PATH_MAX];
    if (fcntl(host_fd, F_GETPATH, hostbuf) < 0) return -LINUX_ENOTDIR;
    if (hl_vfs_host_to_guest(hostbuf, out, out_sz) < 0) return -LINUX_ENOTDIR;
    return 0;
}

int hl_vfs_resolve_at(int dirfd_guest, const char *guest_path,
                      int follow_final_symlink, int create_mode,
                      hl_vfs_resolve_t *out) {
    if (!guest_path || !out) return -LINUX_EFAULT;
    /* Linux returns ENOENT for an empty pathname on every path syscall.
     * join_path() left it as the CWD, so rmdir("") removed the guest's
     * working directory and open("") returned a directory fd.
     * AT_EMPTY_PATH callers are handled before they reach the resolver. */
    if (guest_path[0] == '\0') return -LINUX_ENOENT;
    memset(out, 0, sizeof(*out));

    hl_vfs_t *v = hl_vfs_get();

    /* Legacy mode: do not rewrite — caller uses old paths. */
    if (v->mode == HL_FS_LEGACY) {
        snprintf(out->guest_abs, sizeof(out->guest_abs), "%s", guest_path);
        snprintf(out->host_path, sizeof(out->host_path), "%s", guest_path);
        out->phase = 0;
        return 0;
    }

    char base[HL_VFS_PATH_MAX];
    if (guest_path[0] == '/') {
        snprintf(base, sizeof(base), "%s", guest_path);
    } else if (dirfd_guest == LINUX_AT_FDCWD) {
        char cwd_snap[HL_VFS_PATH_MAX];
        hl_vfs_cwd_copy(cwd_snap, sizeof(cwd_snap));
        join_path(base, sizeof(base), cwd_snap, guest_path);
    } else {
        char dirbase[HL_VFS_PATH_MAX];
        int drc = dirfd_guest_path(dirfd_guest, dirbase, sizeof(dirbase));
        if (drc < 0) return drc;
        join_path(base, sizeof(base), dirbase, guest_path);
    }

    if (normalize_guest(base) < 0) return -LINUX_EINVAL;
    snprintf(out->guest_abs, sizeof(out->guest_abs), "%s", base);

    /* Symlink walk (bounded). For create_mode, do not require final exists. */
    int links = 0;
    while (links < HL_VFS_MAX_SYMLINKS) {
        const hl_mount_t *m = find_mount(v, out->guest_abs);
        out->mount = m;
        if (!m) {
            /* "/" and the interior directories leading to a mount must
             * exist: Linux always has a root, and file dialogs stat("/")
             * and every parent on the way down. Returning ENOENT for them
             * made chdir("/"), stat("/") and opendir("/") fail in the
             * default profile. Synthesize them as read-only virtual
             * directories so they are visible but not mutable — the
             * HL_MOUNT_VIRTUAL flag stops callers handing the guest path
             * to a host mutation syscall. */
            if (mount_ancestor(v, out->guest_abs)) {
                out->mount = &vfs_synthetic_dir_mount;
                out->is_virtual = 1;
                snprintf(out->host_path, sizeof(out->host_path), "%s",
                         out->guest_abs);
                out->phase = 2;
                return 0;
            }
            out->phase = 1; /* mount-select fail */
            if (hl_trace_on(HL_TRACE_FS))
                hl_trace(HL_TRACE_FS, "resolve miss guest=%s", out->guest_abs);
            return -LINUX_ENOENT;
        }
        if (m->flags & HL_MOUNT_VIRTUAL) {
            out->is_virtual = 1;
            snprintf(out->host_path, sizeof(out->host_path), "%s", out->guest_abs);
            out->phase = 2;
            return 0;
        }

        /* Map to host */
        const char *suffix = out->guest_abs + m->guest_len;
        if (*suffix == '/') suffix++;
        /* Truncation here would silently yield a DIFFERENT existing path —
         * typically the parent directory — which unlink/rename/O_TRUNC would
         * then act on. Report ENAMETOOLONG instead. */
        int need;
        if (*suffix)
            need = snprintf(out->host_path, sizeof(out->host_path), "%s/%s",
                            m->host_path, suffix);
        else
            need = snprintf(out->host_path, sizeof(out->host_path), "%s",
                            m->host_path);
        if (need < 0 || (size_t)need >= sizeof(out->host_path))
            return -LINUX_ENAMETOOLONG;

        /* Parent + leaf */
        char *slash = strrchr(out->host_path, '/');
        if (slash && slash != out->host_path) {
            size_t pl = (size_t)(slash - out->host_path);
            memcpy(out->host_parent, out->host_path, pl);
            out->host_parent[pl] = '\0';
            snprintf(out->leaf, sizeof(out->leaf), "%s", slash + 1);
        } else {
            snprintf(out->host_parent, sizeof(out->host_parent), "/");
            snprintf(out->leaf, sizeof(out->leaf), "%s", out->host_path);
        }

        /* Containment check.
         *
         * host_path is built by string concatenation, and only the FINAL
         * component is inspected for symlinks below — every interior
         * component is resolved by the host kernel with host semantics. A
         * symlink planted inside a bind (the guest can create one itself
         * via symlinkat) therefore escaped the mount entirely:
         *   ln -s / esc && cat /home/user/esc/etc/passwd
         * read the host's /etc/passwd.
         *
         * Canonicalize the parent directory and require it to stay inside
         * the mount root. The parent is used rather than the full path so
         * that creating a new leaf still works. Mount roots are themselves
         * canonicalized in hl_vfs_add_bind(), so this is a plain prefix
         * test. A parent that does not exist yet cannot have been
         * subverted, so a realpath() failure is not treated as an escape. */
        if (!path_within_mount(out->host_path, out->host_parent, m->host_path)) {
            out->phase = 3; /* containment */
            if (hl_trace_on(HL_TRACE_FS))
                hl_trace(HL_TRACE_FS,
                         "resolve escape guest=%s host=%s mount=%s",
                         out->guest_abs, out->host_path, m->host_path);
            return -LINUX_EACCES;
        }

        struct stat st;
        if (lstat(out->host_path, &st) == 0 && S_ISLNK(st.st_mode)) {
            /* The caller wants the link itself, not its target: readlink,
             * lstat, unlink, rename, symlink, O_NOFOLLOW. Stop here and
             * return the link's own path. Following it here made `rm link`
             * delete the target and readlink() fail with EINVAL. */
            if (!follow_final_symlink)
                break;
            char linkbuf[HL_VFS_PATH_MAX];
            ssize_t nr = readlink(out->host_path, linkbuf, sizeof(linkbuf) - 1);
            if (nr < 0) return -LINUX_EIO;
            linkbuf[nr] = '\0';
            if (linkbuf[0] == '/') {
                /* Guest-absolute semantics: restart at guest root mapping.
                 * Interpret absolute symlink as guest path. */
                snprintf(out->guest_abs, sizeof(out->guest_abs), "%s", linkbuf);
            } else {
                /* Relative to parent guest dir */
                char gparent[HL_VFS_PATH_MAX];
                snprintf(gparent, sizeof(gparent), "%s", out->guest_abs);
                char *gs = strrchr(gparent, '/');
                if (gs && gs != gparent) *gs = '\0';
                else snprintf(gparent, sizeof(gparent), "/");
                join_path(out->guest_abs, sizeof(out->guest_abs), gparent, linkbuf);
            }
            if (normalize_guest(out->guest_abs) < 0) return -LINUX_EINVAL;
            links++;
            continue;
        }
        break;
    }
    if (links >= HL_VFS_MAX_SYMLINKS) return -LINUX_ELOOP;

    out->phase = 3;
    if (hl_trace_on(HL_TRACE_FS))
        hl_trace(HL_TRACE_FS,
                 "resolve guest=%s host=%s mount=%s create=%d",
                 out->guest_abs, out->host_path,
                 out->mount ? out->mount->guest_prefix : "-",
                 create_mode);
    return 0;
}

int hl_vfs_host_to_guest(const char *host_path, char *guest_out, size_t out_sz) {
    if (!host_path || !guest_out || out_sz == 0) return -1;
    hl_vfs_t *v = hl_vfs_get();
    const hl_mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < v->nmounts; i++) {
        const hl_mount_t *m = &v->mounts[i];
        if (m->flags & HL_MOUNT_VIRTUAL) continue;
        if (!m->host_path[0]) continue;
        size_t hl = strlen(m->host_path);
        /* hl_vfs_add_bind strips a trailing '/' from guest_prefix but not
         * from host_path, so ignore it here or the boundary test below
         * never matches. */
        while (hl > 1 && m->host_path[hl - 1] == '/') hl--;
        if (strncmp(host_path, m->host_path, hl) != 0) continue;
        /* A root host mount ("/") covers every path; for anything else
         * require a component boundary so /foo does not match /foobar. */
        if (!(hl == 1 && m->host_path[0] == '/') &&
            host_path[hl] != '\0' && host_path[hl] != '/') continue;
        if (hl > best_len || !best) {
            best = m;
            best_len = hl;
        }
    }
    if (!best) return -1;
    const char *suffix = host_path + best_len;
    if (*suffix == '/') suffix++;
    if (!*suffix)
        snprintf(guest_out, out_sz, "%s", best->guest_prefix);
    else if (best->guest_prefix[0] == '/' && best->guest_prefix[1] == '\0')
        snprintf(guest_out, out_sz, "/%s", suffix);   /* avoid "//path" */
    else
        snprintf(guest_out, out_sz, "%s/%s", best->guest_prefix, suffix);
    return 0;
}

int hl_vfs_chdir(const char *guest_path) {
    hl_vfs_resolve_t r;
    int rc = hl_vfs_resolve_at(LINUX_AT_FDCWD, guest_path, 1, 0, &r);
    if (rc < 0) return rc;
    if (hl_vfs_mode() == HL_FS_LEGACY) {
        /* Preserve old behavior: host chdir */
        if (chdir(guest_path) < 0) return linux_errno();
        char buf[HL_VFS_PATH_MAX];
        if (getcwd(buf, sizeof(buf)))
            hl_vfs_set_cwd(buf);
        return 0;
    }
    if (r.is_virtual) {
        hl_vfs_set_cwd(r.guest_abs);
        return 0;
    }
    struct stat st;
    if (stat(r.host_path, &st) < 0) return linux_errno();
    if (!S_ISDIR(st.st_mode)) return -LINUX_ENOTDIR;
    /* Virtual CWD only — never host chdir in rooted mode. */
    hl_vfs_set_cwd(r.guest_abs);
    if (hl_trace_on(HL_TRACE_FS))
        hl_trace(HL_TRACE_FS, "chdir guest=%s host=%s", r.guest_abs, r.host_path);
    return 0;
}

int hl_vfs_fchdir(int guest_fd) {
    if (guest_fd < 0 || guest_fd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    if (fd_table[guest_fd].type == FD_CLOSED) return -LINUX_EBADF;
    if (hl_vfs_mode() == HL_FS_LEGACY) {
        int h = fd_to_host(guest_fd);
        if (h < 0) return -LINUX_EBADF;
        if (fchdir(h) < 0) return linux_errno();
        return 0;
    }
    /* Linux: fchdir on a non-directory is ENOTDIR. Rooted mode only moved
     * the virtual CWD string, so fchdir(open("file")) "succeeded" and left
     * the process with a CWD that is not a directory. */
    int h = fd_to_host(guest_fd);
    struct stat dst;
    if (h >= 0 && fstat(h, &dst) == 0 && !S_ISDIR(dst.st_mode))
        return -LINUX_ENOTDIR;
    if (fd_table[guest_fd].type != FD_DIR && h < 0)
        return -LINUX_ENOTDIR;

    char dirbase[HL_VFS_PATH_MAX];
    int drc = dirfd_guest_path(guest_fd, dirbase, sizeof(dirbase));
    if (drc < 0) return drc;
    hl_vfs_set_cwd(dirbase);
    return 0;
}

int hl_vfs_getcwd(char *buf, size_t sz) {
    if (!buf || sz == 0) return -LINUX_ERANGE;
    if (hl_vfs_mode() == HL_FS_LEGACY) {
        if (!getcwd(buf, sz)) return linux_errno();
        return 0;
    }
    const char *cwd = hl_vfs_cwd();
    size_t n = strlen(cwd) + 1;
    if (n > sz) return -LINUX_ERANGE;
    memcpy(buf, cwd, n);
    return 0;
}

int hl_vfs_apply_app_profile(const char *host_home) {
    if (!host_home) host_home = getenv("HOME");
    if (!host_home) return -1;
    hl_vfs_set_mode(HL_FS_ROOTED);
    hl_vfs_set_guest_home("/home/user");
    hl_vfs_add_bind("/", "/", 0); /* optional full host root — prefer narrower */
    /* Narrower preferred profile: replace broad / bind with home-centric */
    hl_vfs_t *v = hl_vfs_get();
    v->nmounts = 0; /* reset binds */
    v->next_mount_id = 1;
    hl_vfs_add_bind("/home/user", host_home, 0);
    char p[HL_VFS_PATH_MAX];
    snprintf(p, sizeof(p), "%s/Music", host_home);
    hl_vfs_add_bind("/home/user/Music", p, 0);
    snprintf(p, sizeof(p), "%s/Desktop", host_home);
    hl_vfs_add_bind("/home/user/Desktop", p, 0);
    snprintf(p, sizeof(p), "%s/Documents", host_home);
    hl_vfs_add_bind("/home/user/Documents", p, 0);
    snprintf(p, sizeof(p), "%s/Downloads", host_home);
    hl_vfs_add_bind("/home/user/Downloads", p, 0);
    hl_vfs_add_bind("/Volumes", "/Volumes", 0);
    hl_vfs_add_bind("/dev", "", HL_MOUNT_VIRTUAL);
    hl_vfs_add_bind("/proc", "", HL_MOUNT_VIRTUAL);
    hl_vfs_set_cwd("/home/user");
    return 0;
}

void hl_vfs_fork_export(hl_vfs_t *dst) {
    if (!dst) return;
    pthread_mutex_lock(&vfs_lock);
    *dst = g_vfs;
    pthread_mutex_unlock(&vfs_lock);
}

void hl_vfs_fork_import(const hl_vfs_t *src) {
    if (!src) return;
    pthread_mutex_lock(&vfs_lock);
    g_vfs = *src;
    g_vfs_inited = 1;
    pthread_mutex_unlock(&vfs_lock);
}
