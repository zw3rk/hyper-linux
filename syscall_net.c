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
 * Most values are identical on both platforms. */
static int translate_msg_flags(int linux_flags) {
    /* MSG_DONTWAIT(0x40), MSG_NOSIGNAL(0x4000), MSG_PEEK(0x2),
     * MSG_WAITALL(0x100) — identical on macOS and Linux for common ones.
     * MSG_NOSIGNAL doesn't exist on macOS, but we ignore SIGPIPE via
     * SO_NOSIGPIPE on the socket. */
    return linux_flags & ~0x4000; /* Strip MSG_NOSIGNAL */
}

/* ---------- Syscall implementations ---------- */

int64_t sys_socket(guest_t *g, int domain, int type, int protocol) {
    (void)g;

    int mac_domain = translate_af_to_mac(domain);
    int real_type = extract_sock_type(type);
    int nonblock = extract_sock_nonblock(type);
    int cloexec = extract_sock_cloexec(type);

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
    int gfd1 = fd_alloc(FD_SOCKET, fds[1]);
    if (gfd0 < 0 || gfd1 < 0) {
        close(fds[0]); close(fds[1]);
        return -LINUX_EMFILE;
    }

    int linux_flags = cloexec ? LINUX_O_CLOEXEC : 0;
    fd_table[gfd0].linux_flags = linux_flags;
    fd_table[gfd1].linux_flags = linux_flags;

    int32_t guest_fds[2] = { gfd0, gfd1 };
    if (guest_write(g, sv_gva, guest_fds, 8) < 0)
        return -LINUX_EFAULT;

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

    /* Write back peer address if requested */
    if (addr_gva && addrlen_gva) {
        uint32_t guest_addrlen;
        if (guest_read(g, addrlen_gva, &guest_addrlen, 4) == 0) {
            uint8_t linux_sa[128];
            int out_len = mac_to_linux_sockaddr(
                (struct sockaddr *)&mac_sa, mac_len,
                linux_sa, (uint32_t)sizeof(linux_sa));
            if (out_len > 0) {
                uint32_t write_len = (uint32_t)out_len;
                if (write_len > guest_addrlen) write_len = guest_addrlen;
                guest_write(g, addr_gva, linux_sa, write_len);
                guest_write(g, addrlen_gva, &write_len, 4);
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

    uint32_t write_len = (uint32_t)out_len;
    if (write_len > guest_addrlen) write_len = guest_addrlen;
    if (guest_write(g, addr_gva, linux_sa, write_len) < 0)
        return -LINUX_EFAULT;
    if (guest_write(g, addrlen_gva, &write_len, 4) < 0)
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

    uint32_t write_len = (uint32_t)out_len;
    if (write_len > guest_addrlen) write_len = guest_addrlen;
    if (guest_write(g, addr_gva, linux_sa, write_len) < 0)
        return -LINUX_EFAULT;
    if (guest_write(g, addrlen_gva, &write_len, 4) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_sendto(guest_t *g, int fd, uint64_t buf_gva, uint64_t len,
                   int flags, uint64_t dest_gva, uint32_t addrlen) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf && len > 0) return -LINUX_EFAULT;

    int mac_flags = translate_msg_flags(flags);

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
        return ret < 0 ? linux_errno() : ret;
    } else {
        ssize_t ret = send(host_fd, buf, len, mac_flags);
        return ret < 0 ? linux_errno() : ret;
    }
}

int64_t sys_recvfrom(guest_t *g, int fd, uint64_t buf_gva, uint64_t len,
                     int flags, uint64_t src_gva, uint64_t addrlen_gva) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf && len > 0) return -LINUX_EFAULT;

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
                uint32_t write_len = (uint32_t)out_len;
                if (write_len > guest_addrlen) write_len = guest_addrlen;
                guest_write(g, src_gva, linux_sa, write_len);
                guest_write(g, addrlen_gva, &write_len, 4);
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
    } else if (level == LINUX_IPPROTO_IPV6) {
        mac_level = IPPROTO_IPV6;
        if (optname == LINUX_IPV6_V6ONLY) mac_optname = IPV6_V6ONLY;
    }

    uint8_t optval[256];
    if (optlen > sizeof(optval)) return -LINUX_EINVAL;
    if (optlen > 0 && guest_read(g, optval_gva, optval, optlen) < 0)
        return -LINUX_EFAULT;

    if (setsockopt(host_fd, mac_level, mac_optname, optval,
                   (socklen_t)optlen) < 0)
        return linux_errno();
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

    uint32_t write_len = (uint32_t)mac_optlen;
    if (write_len > guest_optlen) write_len = guest_optlen;
    if (guest_write(g, optval_gva, optval, write_len) < 0)
        return -LINUX_EFAULT;
    if (guest_write(g, optlen_gva, &write_len, 4) < 0)
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

int64_t sys_sendmsg(guest_t *g, int fd, uint64_t msg_gva, int flags) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    linux_msghdr_t lmsg;
    if (guest_read(g, msg_gva, &lmsg, sizeof(lmsg)) < 0)
        return -LINUX_EFAULT;

    int mac_flags = translate_msg_flags(flags);

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
        void *base = guest_ptr(g, guest_iov[i].iov_base);
        if (!base && guest_iov[i].iov_len > 0) return -LINUX_EFAULT;
        host_iov[i].iov_base = base;
        host_iov[i].iov_len = guest_iov[i].iov_len;
    }

    struct msghdr msg = {
        .msg_name = dest_sa,
        .msg_namelen = dest_len,
        .msg_iov = host_iov,
        .msg_iovlen = (int)lmsg.msg_iovlen,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags = 0,
    };

    ssize_t ret = sendmsg(host_fd, &msg, mac_flags);
    return ret < 0 ? linux_errno() : ret;
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
        void *base = guest_ptr(g, guest_iov[i].iov_base);
        if (!base && guest_iov[i].iov_len > 0) return -LINUX_EFAULT;
        host_iov[i].iov_base = base;
        host_iov[i].iov_len = guest_iov[i].iov_len;
    }

    /* Source address buffer */
    struct sockaddr_storage mac_sa;
    socklen_t sa_len = sizeof(mac_sa);

    struct msghdr msg = {
        .msg_name = lmsg.msg_name ? &mac_sa : NULL,
        .msg_namelen = lmsg.msg_name ? sa_len : 0,
        .msg_iov = host_iov,
        .msg_iovlen = (int)lmsg.msg_iovlen,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags = 0,
    };

    ssize_t ret = recvmsg(host_fd, &msg, mac_flags);
    if (ret < 0) return linux_errno();

    /* Write back source address to guest */
    if (lmsg.msg_name && msg.msg_namelen > 0) {
        uint8_t linux_sa[128];
        int out_len = mac_to_linux_sockaddr(
            (struct sockaddr *)&mac_sa, msg.msg_namelen,
            linux_sa, (uint32_t)sizeof(linux_sa));
        if (out_len > 0) {
            uint32_t write_len = (uint32_t)out_len;
            if (write_len > lmsg.msg_namelen) write_len = lmsg.msg_namelen;
            guest_write(g, lmsg.msg_name, linux_sa, write_len);
        }
        /* Update msg_namelen in guest (offset 8 in linux_msghdr_t) */
        uint32_t nl = (uint32_t)msg.msg_namelen;
        guest_write(g, msg_gva + 8, &nl, 4);
    }

    /* Write back msg_controllen = 0 (offset 40 in linux_msghdr_t).
     * We don't forward ancillary data to the host, so the guest must
     * see controllen=0 — otherwise musl's CMSG_FIRSTHDR returns a
     * pointer into the uninitialized cmsg buffer, causing crashes. */
    uint64_t zero64 = 0;
    guest_write(g, msg_gva + 40, &zero64, 8);

    /* Update msg_flags in guest (offset 48 in linux_msghdr_t) */
    int32_t mflags = msg.msg_flags;
    guest_write(g, msg_gva + 48, &mflags, 4);

    return ret;
}
