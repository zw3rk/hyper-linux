/* helper-abstract-hold.c — bind a Linux abstract AF_UNIX name.
 *
 *   helper-abstract-hold hold <name>   bind, print BOUND, hold ~5s, exit
 *   helper-abstract-hold try  <name>   bind once; print BOUND / EADDRINUSE /
 *                                      ERR:<errno>; exit immediately
 *
 * Used by test-diagnostics.sh (H6): two hl processes sharing $TMPDIR try the
 * same abstract name. The second bind must be refused EADDRINUSE while the
 * first holds it (no name theft), and must succeed once the holder is gone
 * (stale reclaim).
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("usage\n"); return 2; }
    const char *mode = argv[1], *name = argv[2];

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    size_t nl = strlen(name);
    if (nl > sizeof(sa.sun_path) - 2) nl = sizeof(sa.sun_path) - 2;
    sa.sun_path[0] = '\0';
    memcpy(sa.sun_path + 1, name, nl);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { printf("ERR:%d\n", errno); return 1; }
    errno = 0;
    if (bind(s, (struct sockaddr *)&sa, len) != 0) {
        if (errno == EADDRINUSE) printf("EADDRINUSE\n");
        else printf("ERR:%d\n", errno);
        return 1;
    }
    listen(s, 4);
    printf("BOUND\n");
    if (strcmp(mode, "hold") == 0)
        sleep(5);               /* keep the name occupied */
    close(s);
    return 0;
}
