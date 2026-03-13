/* syscall_net.c — Socket/networking syscalls for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Translates Linux aarch64 socket syscalls into macOS equivalents.
 * Key differences handled:
 *   - Address families: Linux AF_INET6=10, macOS AF_INET6=30
 *   - sockaddr layout: macOS has sa_len byte, Linux does not
 *   - Socket type flags: Linux packs SOCK_NONBLOCK/SOCK_CLOEXEC into type
 *   - Socket options: SOL_SOCKET option constants differ
 */
#include "syscall_net.h"
#include "syscall_internal.h"
#include "syscall_signal.h"  /* signal_queue for SIGPIPE */
#include "syscall_io.h"      /* rosettad_is_socket */
#include "guest.h"           /* guest_ptr_avail */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/uio.h>

/* ---------- Address family translation ---------- */

static int translate_af_to_mac(int linux_af) {
    switch (linux_af) {
    case LINUX_AF_UNSPEC: return AF_UNSPEC;
    case LINUX_AF_UNIX:   return AF_UNIX;
    case LINUX_AF_INET:   return AF_INET;
    case LINUX_AF_INET6:  return AF_INET6;
    default:              return linux_af; /* Best effort passthrough */
    }
}

static int translate_af_to_linux(int mac_af) {
    switch (mac_af) {
    case AF_UNSPEC: return LINUX_AF_UNSPEC;
    case AF_UNIX:   return LINUX_AF_UNIX;
    case AF_INET:   return LINUX_AF_INET;
    case AF_INET6:  return LINUX_AF_INET6;
    default:        return mac_af;
    }
}

/* ---------- sockaddr translation ---------- */

/* Linux sockaddr: [u16 sa_family][data...]
 * macOS sockaddr: [u8 sa_len][u8 sa_family][data...]
 * The data portion is identical for AF_INET/AF_INET6/AF_UNIX. */

/* Convert Linux sockaddr (from guest memory) to macOS sockaddr.
 * Returns the macOS sockaddr length, or -1 on error. */
static int linux_to_mac_sockaddr(const void *linux_sa, uint32_t linux_len,
                                  struct sockaddr_storage *mac_sa) {
    if (linux_len < 2) return -1;

    const uint8_t *src = linux_sa;
    uint16_t linux_family;
    memcpy(&linux_family, src, 2);

    int mac_family = translate_af_to_mac((int)linux_family);

    memset(mac_sa, 0, sizeof(*mac_sa));
    mac_sa->ss_len = (uint8_t)linux_len;
    mac_sa->ss_family = (uint8_t)mac_family;

    /* Copy remaining data after the 2-byte family field */
    uint32_t data_len = linux_len - 2;
    if (data_len > sizeof(*mac_sa) - 2) data_len = sizeof(*mac_sa) - 2;
    memcpy((uint8_t *)mac_sa + 2, src + 2, data_len);

    return (int)linux_len;
}

/* Convert macOS sockaddr to Linux sockaddr (to write into guest memory).
 * Returns the Linux sockaddr length, or -1 on error. */
static int mac_to_linux_sockaddr(const struct sockaddr *mac_sa,
                                  socklen_t mac_len,
                                  uint8_t *linux_sa, uint32_t linux_buf_len) {
    if (mac_len < 2 || linux_buf_len < 2) return -1;

    int linux_family = translate_af_to_linux(mac_sa->sa_family);
    uint16_t fam16 = (uint16_t)linux_family;

    /* Write 2-byte family */
    memcpy(linux_sa, &fam16, 2);

    /* Copy remaining data */
    uint32_t data_len = (uint32_t)mac_len - 2;
    if (data_len > linux_buf_len - 2) data_len = linux_buf_len - 2;
    memcpy(linux_sa + 2, (const uint8_t *)mac_sa + 2, data_len);

    return (int)(2 + data_len);
}

/* ---------- Socket type/flags extraction ---------- */

/* Linux encodes SOCK_NONBLOCK and SOCK_CLOEXEC into the type argument. */
static int extract_sock_type(int linux_type) {
    return linux_type & 0xF; /* Lower bits = actual type */
}

static int extract_sock_nonblock(int linux_type) {
    return (linux_type & LINUX_SOCK_NONBLOCK) != 0;
}

static int extract_sock_cloexec(int linux_type) {
    return (linux_type & LINUX_SOCK_CLOEXEC) != 0;
}

/* ---------- Socket option translation ---------- */

/* Translate Linux SOL_SOCKET option names to macOS equivalents.
 * Returns macOS option name, or -1 if unknown. */
static int translate_sockopt(int linux_optname) {
    switch (linux_optname) {
    case LINUX_SO_DEBUG:      return SO_DEBUG;
    case LINUX_SO_REUSEADDR:  return SO_REUSEADDR;
    case LINUX_SO_TYPE:       return SO_TYPE;
    case LINUX_SO_ERROR:      return SO_ERROR;
    case LINUX_SO_DONTROUTE:  return SO_DONTROUTE;
    case LINUX_SO_BROADCAST:  return SO_BROADCAST;
    case LINUX_SO_SNDBUF:     return SO_SNDBUF;
    case LINUX_SO_RCVBUF:     return SO_RCVBUF;
    case LINUX_SO_KEEPALIVE:  return SO_KEEPALIVE;
    case LINUX_SO_OOBINLINE:  return SO_OOBINLINE;
    case LINUX_SO_LINGER:     return SO_LINGER;
    case LINUX_SO_RCVTIMEO:   return SO_RCVTIMEO;
    case LINUX_SO_SNDTIMEO:   return SO_SNDTIMEO;
    case LINUX_SO_ACCEPTCONN: return SO_ACCEPTCONN;
    case LINUX_SO_REUSEPORT:  return SO_REUSEPORT;
    case LINUX_SO_RCVLOWAT:  return SO_RCVLOWAT;
    case LINUX_SO_SNDLOWAT:  return SO_SNDLOWAT;
    default:                  return -1;
    }
}

/* Translate Linux MSG_* flags to macOS equivalents.
 * Several flags have DIFFERENT numeric values on Linux vs macOS:
 *   Flag            Linux   macOS
 *   MSG_OOB         0x01    0x01    (same)
 *   MSG_PEEK        0x02    0x02    (same)
 *   MSG_DONTROUTE   0x04    0x04    (same)
 *   MSG_CTRUNC      0x08    0x20    (output only — not translated for send)
 *   MSG_TRUNC       0x20    0x10    (output only — not translated for send)
 *   MSG_DONTWAIT    0x40    0x80
 *   MSG_EOR         0x80    0x08
 *   MSG_WAITALL     0x100   0x40
 *   MSG_NOSIGNAL    0x4000  N/A     (handled via SO_NOSIGPIPE)
 *   MSG_CMSG_CLOEXEC 0x40000000  N/A (handled by fd table)
 */
static int translate_msg_flags(int linux_flags) {
    int mac_flags = 0;
    if (linux_flags & 0x01)    mac_flags |= MSG_OOB;
    if (linux_flags & 0x02)    mac_flags |= MSG_PEEK;
    if (linux_flags & 0x04)    mac_flags |= MSG_DONTROUTE;
    if (linux_flags & 0x40)    mac_flags |= MSG_DONTWAIT;   /* 0x40 → 0x80 */
    if (linux_flags & 0x80)    mac_flags |= MSG_EOR;        /* 0x80 → 0x08 */
    if (linux_flags & 0x100)   mac_flags |= MSG_WAITALL;    /* 0x100 → 0x40 */
    /* MSG_NOSIGNAL(0x4000): stripped, handled via SO_NOSIGPIPE on socket */
    /* MSG_CMSG_CLOEXEC(0x40000000): stripped, handled by fd table CLOEXEC */
    return mac_flags;
}

/* Translate macOS MSG_* flags back to Linux values (for recvmsg msg_flags). */
static int mac_to_linux_msg_flags(int mac_flags) {
    int linux_flags = 0;
    if (mac_flags & MSG_OOB)       linux_flags |= 0x01;
    if (mac_flags & MSG_PEEK)      linux_flags |= 0x02;
    if (mac_flags & MSG_DONTROUTE) linux_flags |= 0x04;
    if (mac_flags & MSG_CTRUNC)    linux_flags |= 0x08;     /* 0x20 → 0x08 */
    if (mac_flags & MSG_TRUNC)     linux_flags |= 0x20;     /* 0x10 → 0x20 */
    if (mac_flags & MSG_DONTWAIT)  linux_flags |= 0x40;     /* 0x80 → 0x40 */
    if (mac_flags & MSG_EOR)       linux_flags |= 0x80;     /* 0x08 → 0x80 */
    if (mac_flags & MSG_WAITALL)   linux_flags |= 0x100;    /* 0x40 → 0x100 */
    return linux_flags;
}

/* ---------- Syscall implementations ---------- */

int64_t sys_socket(guest_t *g, int domain, int type, int protocol) {
    (void)g;

    int mac_domain = translate_af_to_mac(domain);
    int real_type = extract_sock_type(type);
    int nonblock = extract_sock_nonblock(type);
    int cloexec = extract_sock_cloexec(type);

    /* Rosetta uses AF_UNIX SOCK_SEQPACKET to connect to rosettad for AOT
     * (ahead-of-time) translation. macOS doesn't support SOCK_SEQPACKET
     * for AF_UNIX. Instead, create a socketpair(SOCK_STREAM) and spawn
     * our rosettad handler thread on one end. Rosetta gets the other end.
     * When rosetta later calls connect(), we return success immediately
     * (the socketpair is already connected). */
    if (real_type == 5 /* SOCK_SEQPACKET */ && mac_domain == AF_UNIX) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0)
            return linux_errno();

        /* pair[0] = rosetta's end, pair[1] = our handler thread's end */
        if (nonblock) {
            int fl = fcntl(pair[0], F_GETFL);
            if (fl >= 0) fcntl(pair[0], F_SETFL, fl | O_NONBLOCK);
        }
        if (cloexec) fcntl(pair[0], F_SETFD, FD_CLOEXEC);

        int one = 1;
        setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));

        int gfd = fd_alloc(FD_SOCKET, pair[0]);
        if (gfd < 0) { close(pair[0]); close(pair[1]); return -LINUX_EMFILE; }

        int linux_flags_val = cloexec ? LINUX_O_CLOEXEC : 0;
        fd_table[gfd].linux_flags = linux_flags_val;

        /* Start the rosettad handler thread on pair[1] and track pair[0]
         * so connect() can be intercepted. */
        rosettad_start_handler(pair[1], pair[0]);

        if (g->verbose)
            fprintf(stderr, "hl: rosettad: SEQPACKET → socketpair "
                    "(client_fd=%d guest=%d, handler_fd=%d)\n",
                    pair[0], gfd, pair[1]);
        return gfd;
    }

    int fd = socket(mac_domain, real_type, protocol);
    if (fd < 0) return linux_errno();

    /* Apply SOCK_NONBLOCK */
    if (nonblock) {
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Apply SOCK_CLOEXEC */
    if (cloexec) fcntl(fd, F_SETFD, FD_CLOEXEC);

    /* Suppress SIGPIPE on this socket (macOS-specific) */
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));

    int gfd = fd_alloc(FD_SOCKET, fd);
    if (gfd < 0) { close(fd); return -LINUX_EMFILE; }

    int linux_flags = 0;
    if (cloexec) linux_flags |= LINUX_O_CLOEXEC;
    fd_table[gfd].linux_flags = linux_flags;

    return gfd;
}

int64_t sys_socketpair(guest_t *g, int domain, int type, int protocol,
                       uint64_t sv_gva) {
    int mac_domain = translate_af_to_mac(domain);
    int real_type = extract_sock_type(type);
    int nonblock = extract_sock_nonblock(type);
    int cloexec = extract_sock_cloexec(type);

    int fds[2];
    if (socketpair(mac_domain, real_type, protocol, fds) < 0)
        return linux_errno();

    /* Apply flags */
    for (int i = 0; i < 2; i++) {
        if (nonblock) {
            int fl = fcntl(fds[i], F_GETFL);
            if (fl >= 0) fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
        }
        if (cloexec) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fds[i], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }

    int gfd0 = fd_alloc(FD_SOCKET, fds[0]);
    if (gfd0 < 0) {
        close(fds[0]); close(fds[1]);
        return -LINUX_EMFILE;
    }
    int gfd1 = fd_alloc(FD_SOCKET, fds[1]);
    if (gfd1 < 0) {
        fd_mark_closed(gfd0); close(fds[0]); close(fds[1]);
        return -LINUX_EMFILE;
    }

    int linux_flags = cloexec ? LINUX_O_CLOEXEC : 0;
    fd_table[gfd0].linux_flags = linux_flags;
    fd_table[gfd1].linux_flags = linux_flags;

    int32_t guest_fds[2] = { gfd0, gfd1 };
    if (guest_write(g, sv_gva, guest_fds, 8) < 0) {
        fd_mark_closed(gfd0); close(fds[0]);
        fd_mark_closed(gfd1); close(fds[1]);
        return -LINUX_EFAULT;
    }

    return 0;
}

int64_t sys_bind(guest_t *g, int fd, uint64_t addr_gva, uint32_t addrlen) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    uint8_t linux_sa[128];
    if (addrlen > sizeof(linux_sa)) return -LINUX_EINVAL;
    if (guest_read(g, addr_gva, linux_sa, addrlen) < 0) return -LINUX_EFAULT;

    struct sockaddr_storage mac_sa;
    int mac_len = linux_to_mac_sockaddr(linux_sa, addrlen, &mac_sa);
    if (mac_len < 0) return -LINUX_EINVAL;

    if (bind(host_fd, (struct sockaddr *)&mac_sa, (socklen_t)mac_len) < 0)
        return linux_errno();
    return 0;
}

int64_t sys_listen(int fd, int backlog) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    if (listen(host_fd, backlog) < 0)
        return linux_errno();
    return 0;
}

/* Shared implementation for accept and accept4. */
static int64_t do_accept(guest_t *g, int fd, uint64_t addr_gva,
                          uint64_t addrlen_gva, int nonblock, int cloexec) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    struct sockaddr_storage mac_sa;
    socklen_t mac_len = sizeof(mac_sa);

    int new_fd = accept(host_fd, (struct sockaddr *)&mac_sa, &mac_len);
    if (new_fd < 0) return linux_errno();

    if (nonblock) {
        int fl = fcntl(new_fd, F_GETFL);
        if (fl >= 0) fcntl(new_fd, F_SETFL, fl | O_NONBLOCK);
    }
    if (cloexec) fcntl(new_fd, F_SETFD, FD_CLOEXEC);

    int one = 1;
    setsockopt(new_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));

    int gfd = fd_alloc(FD_SOCKET, new_fd);
    if (gfd < 0) { close(new_fd); return -LINUX_EMFILE; }
    fd_table[gfd].linux_flags = cloexec ? LINUX_O_CLOEXEC : 0;

    /* Write back peer address if requested. The accept has already
     * succeeded and gfd is valid; EFAULT here mirrors Linux kernel
     * behavior (close the new fd and return -EFAULT). */
    if (addr_gva && addrlen_gva) {
        uint32_t guest_addrlen;
        if (guest_read(g, addrlen_gva, &guest_addrlen, 4) == 0) {
            uint8_t linux_sa[128];
            int out_len = mac_to_linux_sockaddr(
                (struct sockaddr *)&mac_sa, mac_len,
                linux_sa, (uint32_t)sizeof(linux_sa));
            if (out_len > 0) {
                uint32_t actual_len = (uint32_t)out_len;
                uint32_t write_len = actual_len;
                if (write_len > guest_addrlen) write_len = guest_addrlen;
                /* Write back actual (not truncated) length per Linux semantics */
                if (guest_write(g, addr_gva, linux_sa, write_len) < 0 ||
                    guest_write(g, addrlen_gva, &actual_len, 4) < 0) {
                    close(new_fd);
                    fd_mark_closed(gfd);
                    return -LINUX_EFAULT;
                }
            }
        }
    }

    return gfd;
}

int64_t sys_accept(guest_t *g, int fd, uint64_t addr_gva,
                   uint64_t addrlen_gva) {
    return do_accept(g, fd, addr_gva, addrlen_gva, 0, 0);
}

int64_t sys_accept4(guest_t *g, int fd, uint64_t addr_gva,
                    uint64_t addrlen_gva, int flags) {
    int nonblock = (flags & LINUX_SOCK_NONBLOCK) != 0;
    int cloexec = (flags & LINUX_SOCK_CLOEXEC) != 0;
    return do_accept(g, fd, addr_gva, addrlen_gva, nonblock, cloexec);
}

int64_t sys_connect(guest_t *g, int fd, uint64_t addr_gva, uint32_t addrlen) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    uint8_t linux_sa[128];
    if (addrlen > sizeof(linux_sa)) return -LINUX_EINVAL;
    if (guest_read(g, addr_gva, linux_sa, addrlen) < 0) return -LINUX_EFAULT;

    /* Rosettad socketpair: already connected, return success immediately.
     * The socketpair was created in sys_socket() when SOCK_SEQPACKET was
     * detected; the handler thread is already running on the other end. */
    if (rosettad_is_socket(host_fd)) {
        if (g->verbose)
            fprintf(stderr, "hl: rosettad: connect intercepted (guest_fd=%d, "
                    "host_fd=%d) → success\n", fd, host_fd);
        return 0;
    }

    struct sockaddr_storage mac_sa;
    int mac_len = linux_to_mac_sockaddr(linux_sa, addrlen, &mac_sa);
    if (mac_len < 0) return -LINUX_EINVAL;

    if (connect(host_fd, (struct sockaddr *)&mac_sa, (socklen_t)mac_len) < 0)
        return linux_errno();
    return 0;
}

int64_t sys_getsockname(guest_t *g, int fd, uint64_t addr_gva,
                        uint64_t addrlen_gva) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    struct sockaddr_storage mac_sa;
    socklen_t mac_len = sizeof(mac_sa);

    if (getsockname(host_fd, (struct sockaddr *)&mac_sa, &mac_len) < 0)
        return linux_errno();

    uint32_t guest_addrlen;
    if (guest_read(g, addrlen_gva, &guest_addrlen, 4) < 0)
        return -LINUX_EFAULT;

    uint8_t linux_sa[128];
    int out_len = mac_to_linux_sockaddr(
        (struct sockaddr *)&mac_sa, mac_len,
        linux_sa, (uint32_t)sizeof(linux_sa));
    if (out_len < 0) return -LINUX_EINVAL;

    uint32_t actual_len = (uint32_t)out_len;
    uint32_t write_len = actual_len;
    if (write_len > guest_addrlen) write_len = guest_addrlen;
    if (guest_write(g, addr_gva, linux_sa, write_len) < 0)
        return -LINUX_EFAULT;
    /* Write back actual (not truncated) length per Linux semantics */
    if (guest_write(g, addrlen_gva, &actual_len, 4) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_getpeername(guest_t *g, int fd, uint64_t addr_gva,
                        uint64_t addrlen_gva) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    struct sockaddr_storage mac_sa;
    socklen_t mac_len = sizeof(mac_sa);

    if (getpeername(host_fd, (struct sockaddr *)&mac_sa, &mac_len) < 0)
        return linux_errno();

    uint32_t guest_addrlen;
    if (guest_read(g, addrlen_gva, &guest_addrlen, 4) < 0)
        return -LINUX_EFAULT;

    uint8_t linux_sa[128];
    int out_len = mac_to_linux_sockaddr(
        (struct sockaddr *)&mac_sa, mac_len,
        linux_sa, (uint32_t)sizeof(linux_sa));
    if (out_len < 0) return -LINUX_EINVAL;

    uint32_t actual_len = (uint32_t)out_len;
    uint32_t write_len = actual_len;
    if (write_len > guest_addrlen) write_len = guest_addrlen;
    if (guest_write(g, addr_gva, linux_sa, write_len) < 0)
        return -LINUX_EFAULT;
    /* Write back actual (not truncated) length per Linux semantics */
    if (guest_write(g, addrlen_gva, &actual_len, 4) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_sendto(guest_t *g, int fd, uint64_t buf_gva, uint64_t len,
                   int linux_flags, uint64_t dest_gva, uint32_t addrlen) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    uint64_t avail = 0;
    void *buf = len > 0 ? guest_ptr_avail(g, buf_gva, &avail) : NULL;
    if (!buf && len > 0) return -LINUX_EFAULT;
    if (len > avail) len = avail;

    int mac_flags = translate_msg_flags(linux_flags);
    /* MSG_NOSIGNAL (0x4000): suppress SIGPIPE on EPIPE.
     * macOS has no MSG_NOSIGNAL; we handle it by not queuing SIGPIPE. */
    int suppress_sigpipe = (linux_flags & 0x4000);

    if (dest_gva && addrlen > 0) {
        uint8_t linux_sa[128];
        if (addrlen > sizeof(linux_sa)) return -LINUX_EINVAL;
        if (guest_read(g, dest_gva, linux_sa, addrlen) < 0)
            return -LINUX_EFAULT;

        struct sockaddr_storage mac_sa;
        int mac_len = linux_to_mac_sockaddr(linux_sa, addrlen, &mac_sa);
        if (mac_len < 0) return -LINUX_EINVAL;

        ssize_t ret = sendto(host_fd, buf, len, mac_flags,
                              (struct sockaddr *)&mac_sa, (socklen_t)mac_len);
        if (ret < 0) {
            if (errno == EPIPE && !suppress_sigpipe)
                signal_queue(LINUX_SIGPIPE);
            return linux_errno();
        }
        return ret;
    } else {
        ssize_t ret = send(host_fd, buf, len, mac_flags);
        if (ret < 0) {
            if (errno == EPIPE && !suppress_sigpipe)
                signal_queue(LINUX_SIGPIPE);
            return linux_errno();
        }
        return ret;
    }
}

int64_t sys_recvfrom(guest_t *g, int fd, uint64_t buf_gva, uint64_t len,
                     int flags, uint64_t src_gva, uint64_t addrlen_gva) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    uint64_t avail = 0;
    void *buf = len > 0 ? guest_ptr_avail(g, buf_gva, &avail) : NULL;
    if (!buf && len > 0) return -LINUX_EFAULT;
    if (len > avail) len = avail;

    int mac_flags = translate_msg_flags(flags);

    struct sockaddr_storage mac_sa;
    socklen_t mac_len = sizeof(mac_sa);

    ssize_t ret;
    if (src_gva && addrlen_gva) {
        ret = recvfrom(host_fd, buf, len, mac_flags,
                       (struct sockaddr *)&mac_sa, &mac_len);
    } else {
        ret = recv(host_fd, buf, len, mac_flags);
    }
    if (ret < 0) return linux_errno();

    /* Write back source address if requested */
    if (src_gva && addrlen_gva && ret >= 0) {
        uint32_t guest_addrlen;
        if (guest_read(g, addrlen_gva, &guest_addrlen, 4) == 0) {
            uint8_t linux_sa[128];
            int out_len = mac_to_linux_sockaddr(
                (struct sockaddr *)&mac_sa, mac_len,
                linux_sa, (uint32_t)sizeof(linux_sa));
            if (out_len > 0) {
                uint32_t actual_len = (uint32_t)out_len;
                uint32_t write_len = actual_len;
                if (write_len > guest_addrlen) write_len = guest_addrlen;
                if (guest_write(g, src_gva, linux_sa, write_len) < 0)
                    return -LINUX_EFAULT;
                /* Write back actual length (Linux returns full size even
                 * if the address was truncated to fit the buffer) */
                if (guest_write(g, addrlen_gva, &actual_len, 4) < 0)
                    return -LINUX_EFAULT;
            }
        }
    }

    return ret;
}

int64_t sys_setsockopt(guest_t *g, int fd, int level, int optname,
                       uint64_t optval_gva, uint32_t optlen) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    int mac_level = level;
    int mac_optname = optname;

    if (level == LINUX_SOL_SOCKET) {
        mac_level = SOL_SOCKET;
        mac_optname = translate_sockopt(optname);
        if (mac_optname < 0) return -LINUX_ENOPROTOOPT;
    } else if (level == LINUX_IPPROTO_TCP) {
        mac_level = IPPROTO_TCP;
        switch (optname) {
        case LINUX_TCP_NODELAY:  mac_optname = TCP_NODELAY; break;
        case LINUX_TCP_KEEPIDLE: mac_optname = TCP_KEEPALIVE; break;
        case LINUX_TCP_KEEPINTVL: mac_optname = 0x101; break; /* TCP_KEEPINTVL */
        case LINUX_TCP_KEEPCNT:  mac_optname = 0x102; break;  /* TCP_KEEPCNT */
        default: break;
        }
    } else if (level == LINUX_IPPROTO_IP) {
        mac_level = IPPROTO_IP;
        if (optname == LINUX_IP_TOS) mac_optname = IP_TOS;
        else if (optname == LINUX_IP_MTU_DISCOVER) {
            /* Linux IP_MTU_DISCOVER controls path MTU discovery strategy.
             * macOS has no equivalent — IP_DONTFRAG is the closest but only
             * works on UDP. Silently succeed as a no-op; the kernel's default
             * PMTU behavior is acceptable for cardano-node P2P networking. */
            return 0;
        }
    } else if (level == LINUX_IPPROTO_IPV6) {
        mac_level = IPPROTO_IPV6;
        if (optname == LINUX_IPV6_V6ONLY) mac_optname = IPV6_V6ONLY;
    }

    uint8_t optval[256];
    if (optlen > sizeof(optval)) return -LINUX_EINVAL;
    if (optlen > 0 && guest_read(g, optval_gva, optval, optlen) < 0)
        return -LINUX_EFAULT;

    if (setsockopt(host_fd, mac_level, mac_optname, optval,
                   (socklen_t)optlen) < 0) {
        if (hl_verbose)
            fprintf(stderr, "hl: setsockopt(fd=%d, level=%d/%d, opt=%d/%d, len=%u): %s\n",
                    fd, level, mac_level, optname, mac_optname, optlen, strerror(errno));
        return linux_errno();
    }
    return 0;
}

int64_t sys_getsockopt(guest_t *g, int fd, int level, int optname,
                       uint64_t optval_gva, uint64_t optlen_gva) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    int mac_level = level;
    int mac_optname = optname;

    if (level == LINUX_SOL_SOCKET) {
        mac_level = SOL_SOCKET;
        mac_optname = translate_sockopt(optname);
        if (mac_optname < 0) return -LINUX_ENOPROTOOPT;
    } else if (level == LINUX_IPPROTO_TCP) {
        mac_level = IPPROTO_TCP;
        switch (optname) {
        case LINUX_TCP_NODELAY:  mac_optname = TCP_NODELAY; break;
        case LINUX_TCP_KEEPIDLE: mac_optname = TCP_KEEPALIVE; break;
        case LINUX_TCP_KEEPINTVL: mac_optname = 0x101; break;
        case LINUX_TCP_KEEPCNT:  mac_optname = 0x102; break;
        default: break;
        }
    } else if (level == LINUX_IPPROTO_IP) {
        mac_level = IPPROTO_IP;
        if (optname == LINUX_IP_TOS) mac_optname = IP_TOS;
        else if (optname == LINUX_IP_MTU_DISCOVER) {
            /* Return IP_PMTUDISC_WANT (3) as a reasonable default */
            uint32_t guest_optlen_val;
            if (guest_read(g, optlen_gva, &guest_optlen_val, 4) < 0)
                return -LINUX_EFAULT;
            if (guest_optlen_val >= sizeof(int)) {
                int val = 3; /* IP_PMTUDISC_WANT */
                if (guest_write(g, optval_gva, &val, sizeof(val)) < 0)
                    return -LINUX_EFAULT;
                uint32_t out_len = sizeof(int);
                if (guest_write(g, optlen_gva, &out_len, 4) < 0)
                    return -LINUX_EFAULT;
            }
            return 0;
        }
    } else if (level == LINUX_IPPROTO_IPV6) {
        mac_level = IPPROTO_IPV6;
        if (optname == LINUX_IPV6_V6ONLY) mac_optname = IPV6_V6ONLY;
    }

    uint32_t guest_optlen;
    if (guest_read(g, optlen_gva, &guest_optlen, 4) < 0)
        return -LINUX_EFAULT;

    uint8_t optval[256];
    socklen_t mac_optlen = (socklen_t)guest_optlen;
    if (mac_optlen > sizeof(optval)) mac_optlen = sizeof(optval);

    if (getsockopt(host_fd, mac_level, mac_optname, optval, &mac_optlen) < 0)
        return linux_errno();

    /* SO_TYPE: macOS returns the raw socket type. On Linux, getsockopt
     * SO_TYPE returns the base type without SOCK_NONBLOCK/SOCK_CLOEXEC
     * flags, and the numeric values happen to match (SOCK_STREAM=1,
     * SOCK_DGRAM=2, SOCK_RAW=3). Strip any flag bits for safety. */
    if (level == LINUX_SOL_SOCKET && optname == LINUX_SO_TYPE
        && mac_optlen >= (socklen_t)sizeof(int)) {
        int *type_val = (int *)optval;
        *type_val &= 0xF;  /* Keep only the base socket type */
    }

    /* SO_ERROR: macOS returns a macOS errno value; translate to Linux. */
    if (level == LINUX_SOL_SOCKET && optname == LINUX_SO_ERROR
        && mac_optlen >= (socklen_t)sizeof(int)) {
        int *err_val = (int *)optval;
        if (*err_val != 0) {
            errno = *err_val;
            *err_val = (int)(-linux_errno());
        }
    }

    /* Write option value, truncating to guest buffer size if needed.
     * Write back actual length (not truncated) per Linux semantics —
     * Linux getsockopt returns the real option size so the caller can
     * detect truncation and retry with a larger buffer. */
    uint32_t actual_len = (uint32_t)mac_optlen;
    uint32_t write_len = actual_len;
    if (write_len > guest_optlen) write_len = guest_optlen;
    if (guest_write(g, optval_gva, optval, write_len) < 0)
        return -LINUX_EFAULT;
    if (guest_write(g, optlen_gva, &actual_len, 4) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_shutdown(int fd, int how) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Shutdown constants are identical on Linux and macOS */
    if (shutdown(host_fd, how) < 0)
        return linux_errno();
    return 0;
}

int64_t sys_sendmsg(guest_t *g, int fd, uint64_t msg_gva, int linux_flags) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    linux_msghdr_t lmsg;
    if (guest_read(g, msg_gva, &lmsg, sizeof(lmsg)) < 0)
        return -LINUX_EFAULT;

    int mac_flags = translate_msg_flags(linux_flags);
    int suppress_sigpipe = (linux_flags & 0x4000);

    /* Translate destination address */
    struct sockaddr_storage mac_sa;
    struct sockaddr *dest_sa = NULL;
    socklen_t dest_len = 0;
    if (lmsg.msg_name && lmsg.msg_namelen > 0) {
        uint8_t linux_sa[128];
        if (lmsg.msg_namelen > sizeof(linux_sa)) return -LINUX_EINVAL;
        if (guest_read(g, lmsg.msg_name, linux_sa, lmsg.msg_namelen) < 0)
            return -LINUX_EFAULT;
        int ml = linux_to_mac_sockaddr(linux_sa, lmsg.msg_namelen, &mac_sa);
        if (ml < 0) return -LINUX_EINVAL;
        dest_sa = (struct sockaddr *)&mac_sa;
        dest_len = (socklen_t)ml;
    }

    /* Build host iovec from guest iovec */
    if (lmsg.msg_iovlen > 64) return -LINUX_EINVAL;

    struct {
        uint64_t iov_base;
        uint64_t iov_len;
    } guest_iov[64];

    if (lmsg.msg_iovlen > 0) {
        if (guest_read(g, lmsg.msg_iov, guest_iov,
                       lmsg.msg_iovlen * 16) < 0)
            return -LINUX_EFAULT;
    }

    struct iovec host_iov[64];
    for (uint64_t i = 0; i < lmsg.msg_iovlen; i++) {
        if (guest_iov[i].iov_len == 0) {
            host_iov[i].iov_base = NULL;
            host_iov[i].iov_len = 0;
            continue;
        }
        uint64_t avail = 0;
        void *base = guest_ptr_avail(g, guest_iov[i].iov_base, &avail);
        if (!base) return -LINUX_EFAULT;
        uint64_t len = guest_iov[i].iov_len;
        if (len > avail) len = avail;
        host_iov[i].iov_base = base;
        host_iov[i].iov_len = len;
    }

    /* Translate control messages from Linux to macOS format.
     *
     * The cmsghdr layout differs between Linux aarch64 and macOS:
     *   Linux:  { uint64_t cmsg_len; int cmsg_level; int cmsg_type; }  (16 bytes)
     *   macOS:  { uint32_t cmsg_len; int cmsg_level; int cmsg_type; }  (12 bytes)
     *
     * CMSG_DATA offset: Linux=16, macOS=12
     * CMSG_ALIGN:       Linux rounds to 8, macOS rounds to 4
     * SOL_SOCKET:       Linux=1, macOS=0xFFFF
     *
     * For SCM_RIGHTS with 1 fd:
     *   Linux:  cmsg_len=20, msg_controllen=24
     *   macOS:  cmsg_len=16, msg_controllen=16
     */
    uint8_t linux_ctrl[512];   /* Linux-format control from guest */
    uint8_t mac_ctrl[256];     /* macOS-format control for host sendmsg */
    uint8_t *ctrl_ptr = NULL;
    socklen_t ctrl_len = 0;

    if (lmsg.msg_control && lmsg.msg_controllen > 0 &&
        lmsg.msg_controllen <= sizeof(linux_ctrl)) {
        if (guest_read(g, lmsg.msg_control, linux_ctrl,
                       lmsg.msg_controllen) < 0)
            return -LINUX_EFAULT;

        /* Walk Linux cmsghdr chain and translate to macOS format.
         * Linux cmsghdr: [8 cmsg_len][4 level][4 type][data at 16]
         * Linux CMSG_ALIGN rounds to 8 bytes. */
        size_t lpos = 0;   /* position in linux_ctrl */
        size_t mpos = 0;   /* position in mac_ctrl */
        uint64_t lctl_len = lmsg.msg_controllen;

        while (lpos + 16 <= lctl_len) {
            uint64_t lcmsg_len;
            int32_t  lcmsg_level, lcmsg_type;
            memcpy(&lcmsg_len, linux_ctrl + lpos, 8);
            memcpy(&lcmsg_level, linux_ctrl + lpos + 8, 4);
            memcpy(&lcmsg_type, linux_ctrl + lpos + 12, 4);

            if (lcmsg_len < 16) break;  /* invalid */
            /* Validate ldata_len fits within remaining input buffer to
             * prevent memcpy overflow from malformed guest cmsghdr. */
            if (lcmsg_len > lctl_len - lpos) break;
            size_t ldata_len = (size_t)(lcmsg_len - 16);

            /* Translate SOL_SOCKET (Linux=1 → macOS=0xFFFF) */
            int mac_level = (lcmsg_level == 1) ? SOL_SOCKET : lcmsg_level;
            int mac_type = lcmsg_type;

            /* Build macOS cmsghdr: [4 cmsg_len][4 level][4 type][data at 12] */
            if (mpos + CMSG_SPACE(ldata_len) > sizeof(mac_ctrl)) break;

            /* Use CMSG macros to build properly aligned macOS cmsghdr */
            struct msghdr tmsg = { .msg_control = mac_ctrl + mpos,
                                   .msg_controllen = sizeof(mac_ctrl) - mpos };
            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&tmsg);
            if (!cmsg) break;
            cmsg->cmsg_len = CMSG_LEN(ldata_len);
            cmsg->cmsg_level = mac_level;
            cmsg->cmsg_type = mac_type;
            memcpy(CMSG_DATA(cmsg), linux_ctrl + lpos + 16, ldata_len);

            /* For SCM_RIGHTS: translate guest fds to host fds.
             * Validate ALL fds before overwriting any — a partial translation
             * would leave the buffer in an inconsistent state on error. */
            if (mac_level == SOL_SOCKET && mac_type == SCM_RIGHTS) {
                int *fds = (int *)CMSG_DATA(cmsg);
                size_t nfds = ldata_len / sizeof(int);
                for (size_t i = 0; i < nfds; i++) {
                    if (fd_to_host(fds[i]) < 0) return -LINUX_EBADF;
                }
                for (size_t i = 0; i < nfds; i++) {
                    fds[i] = fd_to_host(fds[i]);
                }
            }

            mpos += CMSG_SPACE(ldata_len);

            /* Advance to next Linux cmsghdr (8-byte aligned) */
            lpos += (size_t)((lcmsg_len + 7) & ~7ULL);
        }

        if (mpos > 0) {
            ctrl_ptr = mac_ctrl;
            ctrl_len = (socklen_t)mpos;
        }
    }

    struct msghdr msg = {
        .msg_name = dest_sa,
        .msg_namelen = dest_len,
        .msg_iov = host_iov,
        .msg_iovlen = (int)lmsg.msg_iovlen,
        .msg_control = ctrl_ptr,
        .msg_controllen = ctrl_len,
        .msg_flags = 0,
    };

    ssize_t ret = sendmsg(host_fd, &msg, mac_flags);
    if (ret < 0) {
        if (errno == EPIPE && !suppress_sigpipe)
            signal_queue(LINUX_SIGPIPE);
        return linux_errno();
    }
    return ret;
}

int64_t sys_recvmsg(guest_t *g, int fd, uint64_t msg_gva, int flags) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    linux_msghdr_t lmsg;
    if (guest_read(g, msg_gva, &lmsg, sizeof(lmsg)) < 0)
        return -LINUX_EFAULT;

    int mac_flags = translate_msg_flags(flags);

    /* Build host iovec from guest iovec */
    if (lmsg.msg_iovlen > 64) return -LINUX_EINVAL;

    struct {
        uint64_t iov_base;
        uint64_t iov_len;
    } guest_iov[64];

    if (lmsg.msg_iovlen > 0) {
        if (guest_read(g, lmsg.msg_iov, guest_iov,
                       lmsg.msg_iovlen * 16) < 0)
            return -LINUX_EFAULT;
    }

    struct iovec host_iov[64];
    for (uint64_t i = 0; i < lmsg.msg_iovlen; i++) {
        if (guest_iov[i].iov_len == 0) {
            host_iov[i].iov_base = NULL;
            host_iov[i].iov_len = 0;
            continue;
        }
        uint64_t avail = 0;
        void *base = guest_ptr_avail(g, guest_iov[i].iov_base, &avail);
        if (!base) return -LINUX_EFAULT;
        uint64_t len = guest_iov[i].iov_len;
        if (len > avail) len = avail;
        host_iov[i].iov_base = base;
        host_iov[i].iov_len = len;
    }

    /* Source address buffer */
    struct sockaddr_storage mac_sa;
    socklen_t sa_len = sizeof(mac_sa);

    /* Provide a control message buffer for SCM_RIGHTS reception.
     * Rosetta uses SCM_RIGHTS to receive AOT translation fds from
     * the rosettad handler thread via the socketpair.
     *
     * macOS cmsghdr is 12 bytes, Linux is 16 bytes — we must
     * translate from macOS format back to Linux format before
     * writing to guest memory. */
    uint8_t mac_ctrl[256];
    socklen_t ctrl_alloc = 0;

    if (lmsg.msg_control && lmsg.msg_controllen > 0) {
        ctrl_alloc = sizeof(mac_ctrl);
    }

    struct msghdr msg = {
        .msg_name = lmsg.msg_name ? &mac_sa : NULL,
        .msg_namelen = lmsg.msg_name ? sa_len : 0,
        .msg_iov = host_iov,
        .msg_iovlen = (int)lmsg.msg_iovlen,
        .msg_control = ctrl_alloc > 0 ? mac_ctrl : NULL,
        .msg_controllen = ctrl_alloc,
        .msg_flags = 0,
    };

    ssize_t ret = recvmsg(host_fd, &msg, mac_flags);
    if (ret < 0) return linux_errno();

    /* Write back source address to guest */
    if (lmsg.msg_name) {
        if (msg.msg_namelen > 0) {
            uint8_t linux_sa[128];
            int out_len = mac_to_linux_sockaddr(
                (struct sockaddr *)&mac_sa, msg.msg_namelen,
                linux_sa, (uint32_t)sizeof(linux_sa));
            if (out_len > 0) {
                uint32_t write_len = (uint32_t)out_len;
                if (write_len > lmsg.msg_namelen) write_len = lmsg.msg_namelen;
                if (guest_write(g, lmsg.msg_name, linux_sa, write_len) < 0)
                    return -LINUX_EFAULT;
            }
        }
        /* Always write back msg_namelen (even if 0, e.g. SOCK_STREAM) */
        uint32_t nl = (uint32_t)msg.msg_namelen;
        if (guest_write(g, msg_gva + 8, &nl, 4) < 0)
            return -LINUX_EFAULT;
    }

    /* Translate received macOS cmsghdr chain → Linux format.
     *
     * macOS: { uint32_t cmsg_len; int level; int type; data at 12 }
     * Linux: { uint64_t cmsg_len; int level; int type; data at 16 }
     *
     * SOL_SOCKET: macOS=0xFFFF → Linux=1
     * CMSG_ALIGN: macOS=4, Linux=8 */
    if (ctrl_alloc > 0 && msg.msg_controllen > 0 && lmsg.msg_control) {
        uint8_t linux_ctrl[512];
        size_t lpos = 0;

        struct cmsghdr *cmsg;
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_len < CMSG_LEN(0)) continue; /* malformed */
            /* Cap data_len to prevent integer overflow in lcmsg_space
             * calculation. Kernel-returned cmsg_len should be sane but
             * defend against edge cases. */
            size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
            if (data_len > sizeof(linux_ctrl) - 16) break;
            /* Linux CMSG_LEN = 16 + data_len */
            uint64_t lcmsg_len = 16 + data_len;
            /* Linux CMSG_SPACE = ALIGN8(lcmsg_len) */
            size_t lcmsg_space = (size_t)((lcmsg_len + 7) & ~7ULL);

            if (lpos + lcmsg_space > sizeof(linux_ctrl)) break;
            if (lpos + lcmsg_space > lmsg.msg_controllen) break;

            /* Translate SOL_SOCKET (macOS=0xFFFF → Linux=1) */
            int32_t llevel = (cmsg->cmsg_level == SOL_SOCKET) ? 1
                             : cmsg->cmsg_level;
            int32_t ltype = cmsg->cmsg_type;

            /* For SCM_RIGHTS: translate host fds → guest fds */
            uint8_t data_copy[128];
            uint8_t *data_src = CMSG_DATA(cmsg);
            if (cmsg->cmsg_level == SOL_SOCKET &&
                cmsg->cmsg_type == SCM_RIGHTS && data_len <= sizeof(data_copy)) {
                memcpy(data_copy, data_src, data_len);
                int *fds = (int *)data_copy;
                size_t nfds = data_len / sizeof(int);
                for (size_t i = 0; i < nfds; i++) {
                    int host_recv_fd = fds[i];
                    int gfd = fd_alloc(FD_REGULAR, fds[i]);
                    if (gfd < 0) {
                        close(fds[i]);
                        fds[i] = -1;
                    } else {
                        fds[i] = gfd;
                        /* MSG_CMSG_CLOEXEC (0x40000000): set CLOEXEC on received fds */
                        if (flags & 0x40000000)
                            fd_table[gfd].linux_flags |= LINUX_O_CLOEXEC;
                        /* Log SCM_RIGHTS fd reception for rosettad debugging */
                        if (g->verbose && rosettad_is_socket(fd_to_host(fd))) {
                            fprintf(stderr, "hl: rosettad: recvmsg SCM_RIGHTS "
                                    "host_fd=%d → guest_fd=%d\n",
                                    host_recv_fd, gfd);
                        }
                    }
                }
                data_src = data_copy;
            }

            /* Build Linux cmsghdr */
            memset(linux_ctrl + lpos, 0, lcmsg_space);
            memcpy(linux_ctrl + lpos, &lcmsg_len, 8);
            memcpy(linux_ctrl + lpos + 8, &llevel, 4);
            memcpy(linux_ctrl + lpos + 12, &ltype, 4);
            memcpy(linux_ctrl + lpos + 16, data_src, data_len);

            lpos += lcmsg_space;
        }

        if (lpos > 0) {
            if (guest_write(g, lmsg.msg_control, linux_ctrl, lpos) < 0)
                return -LINUX_EFAULT;
            uint64_t lctl = lpos;
            if (guest_write(g, msg_gva + 40, &lctl, 8) < 0)
                return -LINUX_EFAULT;
        } else {
            uint64_t zero64 = 0;
            if (guest_write(g, msg_gva + 40, &zero64, 8) < 0)
                return -LINUX_EFAULT;
        }
    } else {
        /* No control messages — write controllen=0 to prevent guest from
         * reading uninitialized cmsg buffer via CMSG_FIRSTHDR. */
        uint64_t zero64 = 0;
        if (guest_write(g, msg_gva + 40, &zero64, 8) < 0)
            return -LINUX_EFAULT;
    }

    /* Update msg_flags in guest (offset 48 in linux_msghdr_t).
     * macOS msg_flags use macOS values — translate to Linux. */
    int32_t mflags = mac_to_linux_msg_flags(msg.msg_flags);
    if (guest_write(g, msg_gva + 48, &mflags, 4) < 0)
        return -LINUX_EFAULT;

    return ret;
}

/* ---------- sendmmsg / recvmmsg ---------- */

/* Linux struct mmsghdr (aarch64):
 *   struct msghdr msg_hdr;   // offset 0, size 56
 *   unsigned int  msg_len;   // offset 56, size 4
 * Total: 60 bytes (padded to 64 on LP64) */
#define LINUX_MMSGHDR_SIZE 64

int64_t sys_sendmmsg(guest_t *g, int fd, uint64_t mmsg_gva,
                     unsigned int vlen, int flags) {
    if (vlen == 0) return 0;
    if (vlen > 1024) vlen = 1024;  /* Linux caps at UIO_MAXIOV */

    unsigned int sent = 0;
    for (unsigned int i = 0; i < vlen; i++) {
        uint64_t hdr_gva = mmsg_gva + (uint64_t)i * LINUX_MMSGHDR_SIZE;
        int64_t ret = sys_sendmsg(g, fd, hdr_gva, flags);
        if (ret < 0) {
            /* Return count sent so far, or error if none */
            return sent > 0 ? (int64_t)sent : ret;
        }
        /* Write msg_len field at offset 56 in mmsghdr */
        uint32_t msg_len = (uint32_t)ret;
        if (guest_write(g, hdr_gva + 56, &msg_len, 4) < 0)
            return sent > 0 ? (int64_t)sent : -LINUX_EFAULT;
        sent++;
    }
    return (int64_t)sent;
}

int64_t sys_recvmmsg(guest_t *g, int fd, uint64_t mmsg_gva,
                     unsigned int vlen, int flags, uint64_t timeout_gva) {
    (void)timeout_gva;  /* Timeout not implemented (rarely used) */
    if (vlen == 0) return 0;
    if (vlen > 1024) vlen = 1024;

    unsigned int received = 0;
    for (unsigned int i = 0; i < vlen; i++) {
        uint64_t hdr_gva = mmsg_gva + (uint64_t)i * LINUX_MMSGHDR_SIZE;
        int64_t ret = sys_recvmsg(g, fd, hdr_gva, flags);
        if (ret < 0) {
            return received > 0 ? (int64_t)received : ret;
        }
        uint32_t msg_len = (uint32_t)ret;
        if (guest_write(g, hdr_gva + 56, &msg_len, 4) < 0)
            return received > 0 ? (int64_t)received : -LINUX_EFAULT;
        received++;
    }
    return (int64_t)received;
}
