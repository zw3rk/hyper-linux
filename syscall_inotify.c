/* syscall_inotify.c — Linux inotify emulation via kqueue EVFILT_VNODE for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Maps inotify operations to macOS kqueue:
 *   inotify_init1()     → kqueue() + self-pipe for poll/epoll readability
 *   inotify_add_watch() → open(O_EVTONLY) + kevent(EVFILT_VNODE, EV_ADD)
 *   inotify_rm_watch()  → kevent(EV_DELETE) + close(host_fd)
 *   read()              → kevent() poll + translate NOTE_* → IN_* events
 *
 * Limitations (acceptable for MVP):
 *   - Directory watches detect changes (via NOTE_WRITE) but cannot always
 *     determine the specific filename — events may lack the name field
 *   - Cookie-based rename correlation (IN_MOVED_FROM/IN_MOVED_TO) not
 *     implemented — rename produces IN_MOVE_SELF instead
 *   - No recursive watching (IN_ISDIR with subdirectories)
 *   - kqueue coalesces events per-fd naturally (EV_CLEAR), matching
 *     inotify's standard event coalescing
 */
#include "syscall_inotify.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "guest.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/event.h>
#include <sys/stat.h>

/* ---------- Linux inotify constants (from linux/inotify.h) ---------- */
#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_NONBLOCK      0x00000800  /* Same as O_NONBLOCK on aarch64 */
#define IN_CLOEXEC       0x00080000  /* Same as O_CLOEXEC on aarch64 */

/* Linux struct inotify_event layout:
 *   int32_t  wd;      watch descriptor
 *   uint32_t mask;     event mask
 *   uint32_t cookie;   rename correlation cookie
 *   uint32_t len;      length of name (incl. NUL + padding)
 *   char     name[];   optional filename (NUL-terminated, padded) */
#define INOTIFY_EVENT_HEADER_SIZE 16  /* wd + mask + cookie + len */

/* ---------- Internal data structures ---------- */

#define INOTIFY_MAX       16    /* Max inotify instances */
#define INOTIFY_WATCHES   64    /* Max watches per instance */
#define INOTIFY_BUFSIZE   4096  /* Internal event buffer size */

typedef struct {
    int      wd;        /* Watch descriptor (1-based, 0 = unused) */
    int      host_fd;   /* Open fd to the watched path (O_EVTONLY) */
    uint32_t mask;      /* Subscribed IN_* events */
    int      is_dir;    /* 1 if watching a directory */
} inotify_watch_t;

typedef struct {
    int    guest_fd;                          /* -1 if slot is unused */
    int    kq_fd;                             /* kqueue fd */
    int    pipe_rd;                           /* Self-pipe read end (poll/epoll) */
    int    pipe_wr;                           /* Self-pipe write end */
    int    wd_counter;                        /* Next WD to allocate (1-based) */
    int    nonblock;                          /* IN_NONBLOCK flag */
    inotify_watch_t watches[INOTIFY_WATCHES]; /* Watch table */
    uint8_t event_buf[INOTIFY_BUFSIZE];       /* Queued inotify events */
    size_t  event_used;                       /* Bytes used in event_buf */
} inotify_instance_t;

static inotify_instance_t inotify_state[INOTIFY_MAX];
static int inotify_inited = 0;

/* ---------- Init / lookup helpers ---------- */

static void inotify_init_once(void) {
    if (!inotify_inited) {
        for (int i = 0; i < INOTIFY_MAX; i++)
            inotify_state[i].guest_fd = -1;
        inotify_inited = 1;
    }
}

static int inotify_find(int guest_fd) {
    for (int i = 0; i < INOTIFY_MAX; i++)
        if (inotify_state[i].guest_fd == guest_fd) return i;
    return -1;
}

static int inotify_slot_alloc(void) {
    for (int i = 0; i < INOTIFY_MAX; i++)
        if (inotify_state[i].guest_fd == -1) return i;
    return -1;
}

/* Find a watch by WD within an instance. Returns index or -1. */
static int watch_find(inotify_instance_t *inst, int wd) {
    for (int i = 0; i < INOTIFY_WATCHES; i++)
        if (inst->watches[i].wd == wd) return i;
    return -1;
}

/* Find a watch by host_fd (for kevent udata matching). Returns index or -1. */
static int watch_find_by_hostfd(inotify_instance_t *inst, int host_fd) {
    for (int i = 0; i < INOTIFY_WATCHES; i++)
        if (inst->watches[i].wd != 0 && inst->watches[i].host_fd == host_fd)
            return i;
    return -1;
}

/* Allocate a free watch slot. Returns index or -1. */
static int watch_slot_alloc(inotify_instance_t *inst) {
    for (int i = 0; i < INOTIFY_WATCHES; i++)
        if (inst->watches[i].wd == 0) return i;
    return -1;
}

/* ---------- Event translation ---------- */

/* Convert Linux IN_* mask to kqueue NOTE_* flags for EVFILT_VNODE.
 * Not all IN_* events have kqueue equivalents. */
static uint32_t in_mask_to_notes(uint32_t mask) {
    uint32_t notes = 0;
    if (mask & (IN_MODIFY | IN_CLOSE_WRITE))
        notes |= NOTE_WRITE;
    if (mask & IN_ATTRIB)
        notes |= NOTE_ATTRIB;
    if (mask & IN_DELETE_SELF)
        notes |= NOTE_DELETE;
    if (mask & IN_MOVE_SELF)
        notes |= NOTE_RENAME;
    /* NOTE_EXTEND covers file growth, map to IN_MODIFY */
    if (mask & IN_MODIFY)
        notes |= NOTE_EXTEND;
    /* NOTE_LINK covers hard link count changes */
    if (mask & (IN_CREATE | IN_DELETE))
        notes |= NOTE_LINK | NOTE_WRITE;
    /* NOTE_REVOKE for unmount-like events */
    notes |= NOTE_REVOKE;
    return notes;
}

/* Convert kqueue NOTE_* fflags to Linux IN_* mask.
 * The watch's subscribed mask filters which events are actually reported. */
static uint32_t notes_to_in_mask(uint32_t fflags, uint32_t subscribed,
                                  int is_dir) {
    uint32_t mask = 0;

    if (fflags & NOTE_WRITE) {
        if (is_dir) {
            /* Directory write = something was created/deleted inside.
             * Report as IN_CREATE|IN_DELETE since we can't distinguish. */
            if (subscribed & IN_CREATE) mask |= IN_CREATE;
            if (subscribed & IN_DELETE) mask |= IN_DELETE;
            if (subscribed & IN_MODIFY) mask |= IN_MODIFY;
        } else {
            mask |= IN_MODIFY;
        }
    }
    if (fflags & NOTE_EXTEND)
        mask |= IN_MODIFY;
    if (fflags & NOTE_ATTRIB)
        mask |= IN_ATTRIB;
    if (fflags & NOTE_DELETE)
        mask |= IN_DELETE_SELF;
    if (fflags & NOTE_RENAME)
        mask |= IN_MOVE_SELF;
    if (fflags & NOTE_REVOKE)
        mask |= IN_DELETE_SELF;  /* Unmount → treat as deletion */
    if (fflags & NOTE_LINK) {
        if (is_dir && (subscribed & (IN_CREATE | IN_DELETE)))
            mask |= (subscribed & (IN_CREATE | IN_DELETE));
    }

    /* Only report events the watch actually subscribed to */
    return mask & subscribed;
}

/* Queue a single inotify event into the instance's buffer.
 * name may be NULL (no filename). Returns 0 on success, -1 if full. */
static int queue_event(inotify_instance_t *inst, int wd, uint32_t mask,
                        uint32_t cookie, const char *name) {
    /* Calculate event size: header + name length (NUL + padding to 4) */
    uint32_t name_len = 0;
    if (name && name[0]) {
        size_t raw = strlen(name) + 1;  /* Include NUL */
        name_len = (uint32_t)((raw + 3) & ~3U);  /* Pad to 4-byte boundary */
    }
    size_t event_size = INOTIFY_EVENT_HEADER_SIZE + name_len;

    if (inst->event_used + event_size > INOTIFY_BUFSIZE)
        return -1;  /* Buffer full — drop event */

    uint8_t *p = inst->event_buf + inst->event_used;

    /* Write header fields (little-endian, matching aarch64) */
    int32_t wd32 = (int32_t)wd;
    memcpy(p + 0,  &wd32,     4);
    memcpy(p + 4,  &mask,     4);
    memcpy(p + 8,  &cookie,   4);
    memcpy(p + 12, &name_len, 4);

    /* Write name if present (zero-padded) */
    if (name_len > 0) {
        memset(p + INOTIFY_EVENT_HEADER_SIZE, 0, name_len);
        memcpy(p + INOTIFY_EVENT_HEADER_SIZE, name, strlen(name));
    }

    inst->event_used += event_size;
    return 0;
}

/* Signal the self-pipe so poll/epoll sees readability. */
static void pipe_signal(inotify_instance_t *inst) {
    uint8_t byte = 1;
    write(inst->pipe_wr, &byte, 1);
}

/* Drain the self-pipe to reset readability. */
static void pipe_drain(inotify_instance_t *inst) {
    uint8_t drain;
    while (read(inst->pipe_rd, &drain, 1) > 0)
        ;
}

/* ---------- Collect events from kqueue ---------- */

/* Poll the kqueue for pending vnode events and translate them into
 * inotify events in the instance buffer. Returns the number of
 * events collected. */
static int collect_events(inotify_instance_t *inst) {
    struct kevent kevs[16];
    struct timespec ts_zero = {0, 0};

    int nev = kevent(inst->kq_fd, NULL, 0, kevs, 16, &ts_zero);
    if (nev <= 0) return 0;

    int collected = 0;
    for (int i = 0; i < nev; i++) {
        int host_fd = (int)kevs[i].ident;
        int widx = watch_find_by_hostfd(inst, host_fd);
        if (widx < 0) continue;

        inotify_watch_t *w = &inst->watches[widx];
        uint32_t in_mask = notes_to_in_mask((uint32_t)kevs[i].fflags,
                                             w->mask, w->is_dir);
        if (in_mask == 0) continue;

        /* Queue event without a filename for file watches. For directory
         * watches, we also omit the filename since kqueue EVFILT_VNODE
         * doesn't tell us which child changed. */
        if (queue_event(inst, w->wd, in_mask, 0, NULL) == 0)
            collected++;
    }

    /* Signal the self-pipe so poll/epoll sees readability */
    if (collected > 0)
        pipe_signal(inst);

    return collected;
}

/* ---------- Public API ---------- */

int64_t sys_inotify_init1(int flags) {
    inotify_init_once();

    int kq = kqueue();
    if (kq < 0) return linux_errno();

    /* Create self-pipe for poll/epoll readiness */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        close(kq);
        return linux_errno();
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    if (flags & IN_CLOEXEC) {
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
        fcntl(kq, F_SETFD, FD_CLOEXEC);
    }

    /* Allocate guest fd — pipe read end is the host_fd so poll/epoll works */
    int gfd = fd_alloc(FD_INOTIFY, pipefd[0]);
    if (gfd < 0) {
        close(kq);
        close(pipefd[0]);
        close(pipefd[1]);
        return -LINUX_EMFILE;
    }

    int slot = inotify_slot_alloc();
    if (slot < 0) {
        close(kq);
        close(pipefd[0]);
        close(pipefd[1]);
        return -LINUX_ENOMEM;
    }

    inotify_instance_t *inst = &inotify_state[slot];
    inst->guest_fd = gfd;
    inst->kq_fd = kq;
    inst->pipe_rd = pipefd[0];
    inst->pipe_wr = pipefd[1];
    inst->wd_counter = 1;  /* WDs are 1-based */
    inst->nonblock = (flags & IN_NONBLOCK) ? 1 : 0;
    inst->event_used = 0;
    memset(inst->watches, 0, sizeof(inst->watches));

    fd_table[gfd].linux_flags = (flags & IN_CLOEXEC) ? LINUX_O_CLOEXEC : 0;

    return gfd;
}

int64_t sys_inotify_add_watch(guest_t *g, int inotify_fd,
                               uint64_t path_gva, uint32_t mask) {
    int slot = inotify_find(inotify_fd);
    if (slot < 0) return -LINUX_EBADF;

    inotify_instance_t *inst = &inotify_state[slot];

    /* Read path from guest memory */
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    /* Open the path for event monitoring. O_EVTONLY is macOS-specific:
     * opens for event notification only, doesn't prevent unmount or
     * require read access to the file contents. */
    int host_fd = open(path, O_EVTONLY);
    if (host_fd < 0) return linux_errno();

    /* Check if it's a directory */
    struct stat st;
    int is_dir = 0;
    if (fstat(host_fd, &st) == 0 && S_ISDIR(st.st_mode))
        is_dir = 1;

    /* Find or allocate a watch slot */
    int widx = watch_slot_alloc(inst);
    if (widx < 0) {
        close(host_fd);
        return -LINUX_ENOSPC;
    }

    int wd = inst->wd_counter++;
    inotify_watch_t *w = &inst->watches[widx];
    w->wd = wd;
    w->host_fd = host_fd;
    w->mask = mask;
    w->is_dir = is_dir;

    /* Register kevent with EVFILT_VNODE. EV_CLEAR makes it re-arm
     * automatically after each kevent() retrieval, matching inotify's
     * continuous monitoring behavior. */
    uint32_t notes = in_mask_to_notes(mask);
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)host_fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR, notes, 0, NULL);
    if (kevent(inst->kq_fd, &kev, 1, NULL, 0, NULL) < 0) {
        int saved = errno;
        close(host_fd);
        w->wd = 0;
        w->host_fd = 0;
        errno = saved;
        return linux_errno();
    }

    return wd;
}

int64_t sys_inotify_rm_watch(int inotify_fd, int wd) {
    int slot = inotify_find(inotify_fd);
    if (slot < 0) return -LINUX_EBADF;

    inotify_instance_t *inst = &inotify_state[slot];
    int widx = watch_find(inst, wd);
    if (widx < 0) return -LINUX_EINVAL;

    inotify_watch_t *w = &inst->watches[widx];

    /* Remove from kqueue */
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)w->host_fd, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
    kevent(inst->kq_fd, &kev, 1, NULL, 0, NULL);  /* Ignore error */

    close(w->host_fd);
    w->wd = 0;
    w->host_fd = 0;
    w->mask = 0;
    w->is_dir = 0;

    return 0;
}

int64_t inotify_read(int guest_fd, guest_t *g, uint64_t buf_gva,
                      uint64_t count) {
    int slot = inotify_find(guest_fd);
    if (slot < 0) return -LINUX_EBADF;

    inotify_instance_t *inst = &inotify_state[slot];

    /* If no buffered events, poll kqueue for new ones */
    if (inst->event_used == 0) {
        int n = collect_events(inst);

        if (n == 0) {
            if (inst->nonblock)
                return -LINUX_EAGAIN;

            /* Blocking read: wait on the kqueue for events.
             * The self-pipe makes poll/select/epoll work, but for direct
             * read() calls we poll the kqueue with a short timeout loop. */
            struct kevent kev;
            struct timespec ts = {1, 0};  /* 1 second timeout */
            int nev = kevent(inst->kq_fd, NULL, 0, &kev, 1, &ts);
            if (nev <= 0)
                return -LINUX_EAGAIN;

            /* Process the received event */
            int host_fd = (int)kev.ident;
            int widx = watch_find_by_hostfd(inst, host_fd);
            if (widx >= 0) {
                inotify_watch_t *w = &inst->watches[widx];
                uint32_t in_mask = notes_to_in_mask((uint32_t)kev.fflags,
                                                     w->mask, w->is_dir);
                if (in_mask != 0) {
                    queue_event(inst, w->wd, in_mask, 0, NULL);
                    pipe_signal(inst);
                }
            }
        }
    }

    if (inst->event_used == 0)
        return -LINUX_EAGAIN;

    /* Copy buffered events to guest, up to count bytes.
     * Only copy whole events (don't split an event across reads). */
    size_t copied = 0;
    size_t pos = 0;

    while (pos < inst->event_used && copied + INOTIFY_EVENT_HEADER_SIZE <= count) {
        /* Read the name_len field to determine this event's total size */
        uint32_t name_len;
        memcpy(&name_len, inst->event_buf + pos + 12, 4);
        size_t event_size = INOTIFY_EVENT_HEADER_SIZE + name_len;

        if (copied + event_size > count)
            break;  /* Would exceed buffer — stop here */

        if (guest_write(g, buf_gva + copied, inst->event_buf + pos,
                         event_size) < 0)
            return -LINUX_EFAULT;

        copied += event_size;
        pos += event_size;
    }

    /* Compact remaining events in the buffer */
    if (pos > 0 && pos < inst->event_used) {
        memmove(inst->event_buf, inst->event_buf + pos,
                inst->event_used - pos);
        inst->event_used -= pos;
    } else if (pos >= inst->event_used) {
        inst->event_used = 0;
    }

    /* Drain self-pipe if buffer is now empty */
    if (inst->event_used == 0)
        pipe_drain(inst);

    return (int64_t)copied;
}

void inotify_close(int guest_fd) {
    int slot = inotify_find(guest_fd);
    if (slot < 0) return;

    inotify_instance_t *inst = &inotify_state[slot];

    /* Close all watch host fds and remove from kqueue */
    for (int i = 0; i < INOTIFY_WATCHES; i++) {
        if (inst->watches[i].wd != 0) {
            struct kevent kev;
            EV_SET(&kev, (uintptr_t)inst->watches[i].host_fd,
                   EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
            kevent(inst->kq_fd, &kev, 1, NULL, 0, NULL);
            close(inst->watches[i].host_fd);
            inst->watches[i].wd = 0;
        }
    }

    /* Close kqueue and pipe write end.
     * pipe_rd is closed by sys_close() as the host_fd. */
    close(inst->kq_fd);
    close(inst->pipe_wr);

    inst->guest_fd = -1;
    inst->event_used = 0;
}
