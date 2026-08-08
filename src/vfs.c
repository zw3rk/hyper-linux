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
    snprintf(v->cwd, sizeof(v->cwd), "%s", guest_abs);
    /* Normalize trailing slashes except root */
    size_t n = strlen(v->cwd);
    while (n > 1 && v->cwd[n - 1] == '/') {
        v->cwd[--n] = '\0';
    }
    return 0;
}

const char *hl_vfs_cwd(void) {
    return hl_vfs_get()->cwd;
}

static void strip_trailing_slash(char *s) {
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
        s[--n] = '\0';
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
    if (host_path)
        snprintf(m->host_path, sizeof(m->host_path), "%s", host_path);
    m->flags = flags;
    m->guest_len = strlen(m->guest_prefix);
    v->nmounts++;

    if (hl_trace_on(HL_TRACE_FS))
        hl_trace(HL_TRACE_FS, "bind guest=%s host=%s flags=0x%x id=%llu",
                 m->guest_prefix, m->host_path, flags,
                 (unsigned long long)m->id);
    return 0;
}

/* Accept "HOST:GUEST" (host path may contain ':') by splitting on last ':',
 * or "GUEST=HOST". */
int hl_vfs_parse_bind_arg(const char *arg) {
    if (!arg || !*arg) return -1;
    const char *eq = strchr(arg, '=');
    if (eq && eq != arg) {
        char guest[HL_VFS_PATH_MAX], host[HL_VFS_PATH_MAX];
        size_t gl = (size_t)(eq - arg);
        if (gl >= sizeof(guest)) return -1;
        memcpy(guest, arg, gl);
        guest[gl] = '\0';
        snprintf(host, sizeof(host), "%s", eq + 1);
        return hl_vfs_add_bind(guest, host, 0);
    }
    /* HOST:GUEST — find last colon that separates host from guest path
     * starting with /. */
    const char *colon = strrchr(arg, ':');
    if (!colon || colon == arg || colon[1] != '/') return -1;
    char host[HL_VFS_PATH_MAX], guest[HL_VFS_PATH_MAX];
    size_t hl = (size_t)(colon - arg);
    if (hl >= sizeof(host)) return -1;
    memcpy(host, arg, hl);
    host[hl] = '\0';
    snprintf(guest, sizeof(guest), "%s", colon + 1);
    return hl_vfs_add_bind(guest, host, 0);
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
        if (n < 256) components[n++] = tok;
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

int hl_vfs_resolve_at(int dirfd_guest, const char *guest_path,
                      int follow_final_symlink, int create_mode,
                      hl_vfs_resolve_t *out) {
    (void)follow_final_symlink;
    if (!guest_path || !out) return -LINUX_EFAULT;
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
        join_path(base, sizeof(base), v->cwd, guest_path);
    } else {
        /* Resolve via dirfd guest path hint if available */
        if (dirfd_guest < 0 || dirfd_guest >= FD_TABLE_SIZE ||
            fd_table[dirfd_guest].type == FD_CLOSED)
            return -LINUX_EBADF;
        const char *hint = NULL;
        if (fd_table[dirfd_guest].of && fd_table[dirfd_guest].of->guest_path_hint)
            hint = fd_table[dirfd_guest].of->guest_path_hint;
        if (!hint)
            return -LINUX_ENOTDIR;
        join_path(base, sizeof(base), hint, guest_path);
    }

    if (normalize_guest(base) < 0) return -LINUX_EINVAL;
    snprintf(out->guest_abs, sizeof(out->guest_abs), "%s", base);

    /* Symlink walk (bounded). For create_mode, do not require final exists. */
    int links = 0;
    while (links < HL_VFS_MAX_SYMLINKS) {
        const hl_mount_t *m = find_mount(v, out->guest_abs);
        out->mount = m;
        if (!m) {
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
        if (*suffix)
            snprintf(out->host_path, sizeof(out->host_path), "%s/%s",
                     m->host_path, suffix);
        else
            snprintf(out->host_path, sizeof(out->host_path), "%s", m->host_path);

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

        /* Follow intermediate symlinks when not create of final */
        struct stat st;
        if (lstat(out->host_path, &st) == 0 && S_ISLNK(st.st_mode)) {
            if (create_mode && links == 0) {
                /* For O_CREAT on final, if final is symlink we still follow
                 * unless O_NOFOLLOW — handled by caller flags. Default follow. */
            }
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
        if (strncmp(host_path, m->host_path, hl) != 0) continue;
        if (host_path[hl] != '\0' && host_path[hl] != '/') continue;
        if (hl > best_len) {
            best = m;
            best_len = hl;
        }
    }
    if (!best) return -1;
    const char *suffix = host_path + best_len;
    if (*suffix == '/') suffix++;
    if (*suffix)
        snprintf(guest_out, out_sz, "%s/%s", best->guest_prefix, suffix);
    else
        snprintf(guest_out, out_sz, "%s", best->guest_prefix);
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
    if (fd_table[guest_fd].of && fd_table[guest_fd].of->guest_path_hint) {
        hl_vfs_set_cwd(fd_table[guest_fd].of->guest_path_hint);
        return 0;
    }
    return -LINUX_ENOENT;
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
