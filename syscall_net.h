/* syscall_net.h — Socket/networking syscalls for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Translates Linux socket syscalls into macOS equivalents, handling the
 * differences in address family constants, sockaddr layout (sa_len byte),
 * socket option constants, and flag encoding (SOCK_NONBLOCK/SOCK_CLOEXEC
 * packed into the type argument).
 */
#ifndef SYSCALL_NET_H
#define SYSCALL_NET_H

#include <stdint.h>
#include "guest.h"

/* ---------- Linux address families ---------- */
#define LINUX_AF_UNSPEC   0
#define LINUX_AF_UNIX     1
#define LINUX_AF_INET     2
#define LINUX_AF_INET6   10

/* ---------- Linux socket types + flags ---------- */
#define LINUX_SOCK_STREAM    1
#define LINUX_SOCK_DGRAM     2
#define LINUX_SOCK_RAW       3
#define LINUX_SOCK_NONBLOCK  0x800
#define LINUX_SOCK_CLOEXEC   0x80000

/* ---------- Linux SOL_SOCKET option level ---------- */
#define LINUX_SOL_SOCKET 1

/* Linux SOL_SOCKET option names (differ from macOS!) */
#define LINUX_SO_DEBUG       1
#define LINUX_SO_REUSEADDR   2
#define LINUX_SO_TYPE        3
#define LINUX_SO_ERROR       4
#define LINUX_SO_DONTROUTE   5
#define LINUX_SO_BROADCAST   6
#define LINUX_SO_SNDBUF      7
#define LINUX_SO_RCVBUF      8
#define LINUX_SO_KEEPALIVE   9
#define LINUX_SO_OOBINLINE  10
#define LINUX_SO_LINGER     13
#define LINUX_SO_RCVTIMEO   20
#define LINUX_SO_SNDTIMEO   21
#define LINUX_SO_ACCEPTCONN 30
#define LINUX_SO_REUSEPORT  15

/* ---------- Linux TCP level options ---------- */
#define LINUX_IPPROTO_TCP    6
#define LINUX_TCP_NODELAY    1

/* ---------- Linux shutdown() how values ---------- */
#define LINUX_SHUT_RD   0
#define LINUX_SHUT_WR   1
#define LINUX_SHUT_RDWR 2

/* ---------- Linux msghdr (aarch64) ---------- */
typedef struct {
    uint64_t msg_name;       /* Guest pointer to sockaddr */
    uint32_t msg_namelen;
    uint32_t _pad0;
    uint64_t msg_iov;        /* Guest pointer to iovec array */
    uint64_t msg_iovlen;
    uint64_t msg_control;    /* Guest pointer to ancillary data */
    uint64_t msg_controllen;
    int32_t  msg_flags;
    int32_t  _pad1;
} linux_msghdr_t;

/* ---------- Socket syscall handlers ---------- */

int64_t sys_socket(guest_t *g, int domain, int type, int protocol);
int64_t sys_socketpair(guest_t *g, int domain, int type, int protocol,
                       uint64_t sv_gva);
int64_t sys_bind(guest_t *g, int fd, uint64_t addr_gva, uint32_t addrlen);
int64_t sys_listen(int fd, int backlog);
int64_t sys_accept(guest_t *g, int fd, uint64_t addr_gva, uint64_t addrlen_gva);
int64_t sys_accept4(guest_t *g, int fd, uint64_t addr_gva,
                    uint64_t addrlen_gva, int flags);
int64_t sys_connect(guest_t *g, int fd, uint64_t addr_gva, uint32_t addrlen);
int64_t sys_getsockname(guest_t *g, int fd, uint64_t addr_gva,
                        uint64_t addrlen_gva);
int64_t sys_getpeername(guest_t *g, int fd, uint64_t addr_gva,
                        uint64_t addrlen_gva);
int64_t sys_sendto(guest_t *g, int fd, uint64_t buf_gva, uint64_t len,
                   int flags, uint64_t dest_gva, uint32_t addrlen);
int64_t sys_recvfrom(guest_t *g, int fd, uint64_t buf_gva, uint64_t len,
                     int flags, uint64_t src_gva, uint64_t addrlen_gva);
int64_t sys_setsockopt(guest_t *g, int fd, int level, int optname,
                       uint64_t optval_gva, uint32_t optlen);
int64_t sys_getsockopt(guest_t *g, int fd, int level, int optname,
                       uint64_t optval_gva, uint64_t optlen_gva);
int64_t sys_shutdown(int fd, int how);
int64_t sys_sendmsg(guest_t *g, int fd, uint64_t msg_gva, int flags);
int64_t sys_recvmsg(guest_t *g, int fd, uint64_t msg_gva, int flags);

#endif /* SYSCALL_NET_H */
