/* test-net.c — Test AF_INET/AF_INET6 networking syscalls
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: TCP self-connect (socket/bind/listen/connect/accept/send/recv),
 *        UDP loopback (sendto/recvfrom), AF_INET6 socket creation,
 *        setsockopt/getsockopt (SO_REUSEADDR, SO_SNDBUF, SO_RCVBUF)
 *
 * Syscalls exercised: socket(198), bind(200), listen(201), connect(203),
 *                     accept(202), sendto(206), recvfrom(207),
 *                     getsockname(204), setsockopt(208), getsockopt(209),
 *                     shutdown(210), close(57)
 */
#include "test-harness.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(void) {
    int passes = 0, fails = 0;

    printf("test-net: AF_INET/AF_INET6 networking tests\n");

    /* Test TCP self-connect: server binds, client connects, exchange data */
    TEST("TCP self-connect");
    {
        int server = socket(AF_INET, SOCK_STREAM, 0);
        if (server < 0) { FAIL("server socket"); goto tcp_done; }

        /* Bind to 127.0.0.1:0 (kernel picks port) */
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            FAIL("bind"); close(server); goto tcp_done;
        }
        if (listen(server, 1) < 0) {
            FAIL("listen"); close(server); goto tcp_done;
        }

        /* Get assigned port */
        socklen_t alen = sizeof(addr);
        getsockname(server, (struct sockaddr *)&addr, &alen);

        /* Connect client */
        int client = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(client, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            FAIL("connect"); close(client); close(server); goto tcp_done;
        }

        /* Accept connection */
        int conn = accept(server, NULL, NULL);
        if (conn < 0) {
            FAIL("accept"); close(client); close(server); goto tcp_done;
        }

        /* Exchange data */
        const char *msg = "hello-tcp";
        send(client, msg, strlen(msg), 0);

        char buf[64];
        ssize_t n = recv(conn, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            if (strcmp(buf, msg) == 0) PASS();
            else FAIL("data mismatch");
        } else FAIL("recv failed");

        close(conn);
        close(client);
        close(server);
    }
    tcp_done:;

    /* Test UDP loopback: send + receive on same socket */
    TEST("UDP loopback");
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) { FAIL("socket"); goto udp_done; }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            FAIL("bind"); close(sock); goto udp_done;
        }

        /* Get assigned port */
        socklen_t alen = sizeof(addr);
        getsockname(sock, (struct sockaddr *)&addr, &alen);

        /* Send to self */
        const char *msg = "hello-udp";
        ssize_t sent = sendto(sock, msg, strlen(msg), 0,
                              (struct sockaddr *)&addr, sizeof(addr));
        if (sent < 0) { FAIL("sendto"); close(sock); goto udp_done; }

        /* Receive */
        char buf[64];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &flen);
        if (n > 0) {
            buf[n] = '\0';
            if (strcmp(buf, msg) == 0) PASS();
            else FAIL("data mismatch");
        } else FAIL("recvfrom failed");

        close(sock);
    }
    udp_done:;

    /* Test AF_INET6 socket creation + bind to ::1 */
    TEST("AF_INET6 socket + bind");
    {
        int sock = socket(AF_INET6, SOCK_STREAM, 0);
        if (sock < 0) { FAIL("socket"); goto ipv6_done; }

        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr = in6addr_loopback;
        addr6.sin6_port = 0;

        if (bind(sock, (struct sockaddr *)&addr6, sizeof(addr6)) == 0) {
            /* Verify getsockname returns AF_INET6 */
            struct sockaddr_in6 out;
            socklen_t olen = sizeof(out);
            getsockname(sock, (struct sockaddr *)&out, &olen);
            if (out.sin6_family == AF_INET6) PASS();
            else FAIL("wrong family");
        } else FAIL("bind ::1");

        close(sock);
    }
    ipv6_done:;

    /* Test setsockopt + getsockopt: SO_REUSEADDR */
    TEST("SO_REUSEADDR set+get");
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            int val = 1;
            if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                           &val, sizeof(val)) == 0) {
                int got = 0;
                socklen_t glen = sizeof(got);
                getsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &got, &glen);
                if (got != 0) PASS();
                else FAIL("SO_REUSEADDR not set");
            } else FAIL("setsockopt failed");
            close(sock);
        } else FAIL("socket failed");
    }

    /* Test SO_SNDBUF get */
    TEST("SO_SNDBUF getsockopt");
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            int val = 0;
            socklen_t vlen = sizeof(val);
            if (getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &val, &vlen) == 0) {
                if (val > 0) PASS();
                else FAIL("SO_SNDBUF is 0");
            } else FAIL("getsockopt failed");
            close(sock);
        } else FAIL("socket failed");
    }

    /* Test SO_RCVBUF get */
    TEST("SO_RCVBUF getsockopt");
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            int val = 0;
            socklen_t vlen = sizeof(val);
            if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &val, &vlen) == 0) {
                if (val > 0) PASS();
                else FAIL("SO_RCVBUF is 0");
            } else FAIL("getsockopt failed");
            close(sock);
        } else FAIL("socket failed");
    }

    printf("\nResults: %d passed, %d failed\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
