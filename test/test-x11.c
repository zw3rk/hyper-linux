/* test-x11.c — Test X11 connectivity via raw wire protocol
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Validates X11 socket transport by speaking raw X11 wire protocol
 * over AF_UNIX or TCP.  Zero external library dependencies — all X11
 * protocol structures are hand-coded.
 *
 * Tests: DISPLAY env var parsing, Xauthority reading, X server socket
 *        connect, window creation + map, Expose event handling + draw.
 *
 * All X11-server-dependent tests skip gracefully when DISPLAY is
 * unset or the X server is unreachable (exit 0, 0 failures).
 *
 * Syscalls exercised: socket(198), connect(203), read(63), write(64),
 *                     ppoll(73), openat(56), close(57)
 */
#include "test-harness.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

/* Test precondition not met — not a failure */
#define SKIP(msg) do { printf("SKIP: %s\n", msg); } while(0)

/* ── Little-endian helpers ───────────────────────────────────────── */

static inline uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Round up to multiple of 4 */
static inline int pad4(int n) { return (n + 3) & ~3; }

/* Read exactly n bytes from fd.  Retries on EINTR.
 * Returns 0 on success, -1 on failure/EOF. */
static int read_exact(int fd, void *buf, int n) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (int)r;
    }
    return 0;
}

/* ── DISPLAY Parsing ─────────────────────────────────────────────── */

typedef enum { DISPLAY_UNIX, DISPLAY_TCP } display_type_t;

typedef struct {
    display_type_t type;
    char socket_path[256]; /* DISPLAY_UNIX: path to Unix domain socket */
    char hostname[256];    /* DISPLAY_TCP: hostname or IP address */
    int display_number;
    int screen_number;
} display_info_t;

/* Parse X11 DISPLAY string into structured info.
 * Supports:  ":N", ":N.S", "host:N", "/path:N"
 * Returns 0 on success, -1 on invalid format. */
static int parse_display(const char *display, display_info_t *info) {
    if (!display || !*display) return -1;
    memset(info, 0, sizeof(*info));

    /* Last ':' separates host/path from display.screen */
    const char *colon = strrchr(display, ':');
    if (!colon || colon == display + strlen(display) - 1) return -1;

    info->display_number = atoi(colon + 1);
    const char *dot = strchr(colon + 1, '.');
    info->screen_number = dot ? atoi(dot + 1) : 0;

    int host_len = (int)(colon - display);

    if (host_len == 0) {
        /* ":N" — standard Unix socket at /tmp/.X11-unix/XN */
        info->type = DISPLAY_UNIX;
        snprintf(info->socket_path, sizeof(info->socket_path),
                 "/tmp/.X11-unix/X%d", info->display_number);
    } else if (display[0] == '/') {
        /* "/path:N" — XQuartz launchd or custom socket path.
         * libxcb convention: the path before ':N' is the socket.
         * Fallback: try the full DISPLAY string as the socket filename
         * (some setups embed ':N' in the actual filename). */
        info->type = DISPLAY_UNIX;
        if (host_len < (int)sizeof(info->socket_path)) {
            memcpy(info->socket_path, display, host_len);
            info->socket_path[host_len] = '\0';
        }
        /* If path-prefix doesn't exist, use full DISPLAY string */
        if (access(info->socket_path, F_OK) != 0)
            snprintf(info->socket_path, sizeof(info->socket_path),
                     "%s", display);
    } else {
        /* "host:N" — TCP connection to host:6000+N */
        info->type = DISPLAY_TCP;
        if (host_len >= (int)sizeof(info->hostname))
            host_len = (int)sizeof(info->hostname) - 1;
        memcpy(info->hostname, display, host_len);
        info->hostname[host_len] = '\0';
    }
    return 0;
}

/* ── Xauthority ──────────────────────────────────────────────────── */

typedef struct {
    char name[64];     /* auth protocol name (e.g. "MIT-MAGIC-COOKIE-1") */
    uint8_t data[256]; /* auth data (16 bytes for MIT-MAGIC-COOKIE-1) */
    uint16_t name_len;
    uint16_t data_len;
} xauth_t;

/* Read 2-byte big-endian value from fd */
static int xauth_read_be16(int fd, uint16_t *out) {
    uint8_t buf[2];
    if (read(fd, buf, 2) != 2) return -1;
    *out = ((uint16_t)buf[0] << 8) | buf[1];
    return 0;
}

/* Skip n bytes in fd */
static int xauth_skip(int fd, uint16_t n) {
    uint8_t tmp[64];
    while (n > 0) {
        int chunk = n > 64 ? 64 : n;
        if (read(fd, tmp, chunk) != chunk) return -1;
        n -= (uint16_t)chunk;
    }
    return 0;
}

/* Find an Xauthority entry matching display_num.
 * Reads from XAUTHORITY env var or $HOME/.Xauthority.
 * Returns 0 if found, -1 if not. */
static int find_xauth(int display_num, xauth_t *auth) {
    const char *path = getenv("XAUTHORITY");
    char pathbuf[512];
    if (!path) {
        const char *home = getenv("HOME");
        if (!home) return -1;
        snprintf(pathbuf, sizeof(pathbuf), "%s/.Xauthority", home);
        path = pathbuf;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    char display_str[16];
    int display_str_len = snprintf(display_str, sizeof(display_str),
                                   "%d", display_num);

    /* Xauthority binary format — repeated entries:
     *   family(2 BE), addr_len(2 BE), addr(addr_len),
     *   number_len(2 BE), number(number_len),
     *   name_len(2 BE), name(name_len),
     *   data_len(2 BE), data(data_len) */
    while (1) {
        uint16_t family, addr_len, number_len, name_len, data_len;

        if (xauth_read_be16(fd, &family) < 0) break;
        if (xauth_read_be16(fd, &addr_len) < 0) break;
        if (xauth_skip(fd, addr_len) < 0) break;
        if (xauth_read_be16(fd, &number_len) < 0) break;

        /* Read display number (ASCII string) */
        char numbuf[32] = {0};
        if (number_len > 0 && number_len < sizeof(numbuf)) {
            if (read(fd, numbuf, number_len) != number_len) break;
        } else if (number_len > 0) {
            if (xauth_skip(fd, number_len) < 0) break;
        }

        /* Read auth protocol name */
        if (xauth_read_be16(fd, &name_len) < 0) break;
        char namebuf[64] = {0};
        if (name_len > 0 && name_len < sizeof(namebuf)) {
            if (read(fd, namebuf, name_len) != name_len) break;
        } else if (name_len > 0) {
            if (xauth_skip(fd, name_len) < 0) break;
        }

        if (xauth_read_be16(fd, &data_len) < 0) break;

        /* Match on display number */
        int match = ((int)number_len == display_str_len &&
                     memcmp(numbuf, display_str, display_str_len) == 0 &&
                     name_len < sizeof(auth->name) &&
                     data_len <= sizeof(auth->data));
        if (match) {
            memcpy(auth->name, namebuf, name_len);
            auth->name_len = name_len;
            auth->data_len = data_len;
            if (data_len > 0 &&
                read(fd, auth->data, data_len) != data_len)
                break;
            close(fd);
            return 0;
        }
        if (xauth_skip(fd, data_len) < 0) break;
    }
    close(fd);
    return -1;
}

/* ── X11 Connection ──────────────────────────────────────────────── */

typedef struct {
    int fd;
    uint32_t id_base;     /* resource ID base */
    uint32_t id_mask;     /* resource ID mask */
    uint32_t root_window;
    uint32_t root_visual;
    uint8_t  root_depth;
    uint32_t white_pixel;
    uint32_t black_pixel;
    uint32_t next_id;     /* counter for resource allocation */
} x11_conn_t;

/* Allocate the next X11 resource ID */
static uint32_t x11_alloc_id(x11_conn_t *c) {
    c->next_id++;
    return c->id_base | (c->next_id & c->id_mask);
}

/* Connect to X server and perform protocol handshake.
 * On success, conn is populated and conn->fd is open.
 * Returns 0 on success, -1 on failure. */
static int x11_connect(const display_info_t *info, const xauth_t *auth,
                        x11_conn_t *conn) {
    int fd = -1;
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;

    if (info->type == DISPLAY_UNIX) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        size_t plen = strlen(info->socket_path);
        if (plen >= sizeof(addr.sun_path))
            plen = sizeof(addr.sun_path) - 1;
        memcpy(addr.sun_path, info->socket_path, plen);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
    } else {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(6000 + info->display_number);
        /* Handle "localhost" specially; otherwise try inet_pton */
        if (strcmp(info->hostname, "localhost") == 0 ||
            strcmp(info->hostname, "127.0.0.1") == 0)
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        else if (inet_pton(AF_INET, info->hostname,
                           &addr.sin_addr) <= 0)
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
    }

    /* X11 connection setup request:
     *   byte_order(1) pad(1) major(2) minor(2)
     *   auth_name_len(2) auth_data_len(2) pad(2)
     *   [auth_name padded] [auth_data padded] */
    uint16_t auth_name_len = auth ? auth->name_len : 0;
    uint16_t auth_data_len = auth ? auth->data_len : 0;
    int setup_len = 12 + pad4(auth_name_len) + pad4(auth_data_len);
    uint8_t setup[512];
    if (setup_len > (int)sizeof(setup)) { close(fd); return -1; }
    memset(setup, 0, sizeof(setup));
    setup[0] = 0x6c; /* little-endian byte order */
    put_le16(setup + 2, 11);  /* protocol major version */
    put_le16(setup + 4, 0);   /* protocol minor version */
    put_le16(setup + 6, auth_name_len);
    put_le16(setup + 8, auth_data_len);
    if (auth_name_len > 0)
        memcpy(setup + 12, auth->name, auth_name_len);
    if (auth_data_len > 0)
        memcpy(setup + 12 + pad4(auth_name_len),
               auth->data, auth_data_len);

    if (write(fd, setup, setup_len) != setup_len) {
        close(fd);
        return -1;
    }

    /* Reply header: status(1) pad(1) major(2) minor(2) length(2) */
    uint8_t hdr[8];
    if (read_exact(fd, hdr, 8) < 0) { close(fd); return -1; }
    if (hdr[0] != 1) {
        /* Failed (0) or authenticate (2) — read remaining then bail */
        int extra = le16(hdr + 6) * 4;
        uint8_t discard[1024];
        if (extra > 0 && extra <= (int)sizeof(discard))
            read_exact(fd, discard, extra);
        close(fd);
        return -1;
    }

    /* Read additional connection data */
    int data_bytes = le16(hdr + 6) * 4;
    uint8_t data[8192];
    if (data_bytes > (int)sizeof(data) ||
        read_exact(fd, data, data_bytes) < 0) {
        close(fd);
        return -1;
    }

    /* Parse fixed fields:
     *   +0: release_number(4)  +4: resource_id_base(4)
     *   +8: resource_id_mask(4) +12: motion_buf_size(4)
     *  +16: vendor_length(2)   +18: max_request_length(2)
     *  +20: num_screens(1)     +21: num_formats(1)
     *  +22..+31: image_byte_order, etc. */
    conn->id_base = le32(data + 4);
    conn->id_mask = le32(data + 8);
    uint16_t vendor_len = le16(data + 16);
    uint8_t num_screens = data[20];
    uint8_t num_formats = data[21];
    if (num_screens == 0) { close(fd); return -1; }

    /* Screen 0 is after 32-byte fixed header + vendor + formats */
    int scr_off = 32 + pad4(vendor_len) + num_formats * 8;
    if (scr_off + 40 > data_bytes) { close(fd); return -1; }

    /* Screen: root(4) colormap(4) white(4) black(4) masks(4)
     *         w_px(2) h_px(2) w_mm(2) h_mm(2) min_maps(2) max_maps(2)
     *         root_visual(4) backing(1) save_unders(1) root_depth(1) */
    uint8_t *scr = data + scr_off;
    conn->root_window = le32(scr + 0);
    conn->white_pixel = le32(scr + 8);
    conn->black_pixel = le32(scr + 12);
    conn->root_visual = le32(scr + 32);
    conn->root_depth  = scr[38];
    conn->fd = fd;
    return 0;
}

/* ── X11 Request Helpers ─────────────────────────────────────────── */

/* CreateGC (opcode 55): create graphics context with foreground color */
static int x11_create_gc(x11_conn_t *c, uint32_t gcid,
                          uint32_t drawable, uint32_t fg) {
    uint8_t req[20];
    req[0] = 55; req[1] = 0;
    put_le16(req + 2, 5);          /* length = 5 dwords */
    put_le32(req + 4, gcid);
    put_le32(req + 8, drawable);
    put_le32(req + 12, 0x04);      /* value_mask: GCForeground */
    put_le32(req + 16, fg);
    return (write(c->fd, req, 20) == 20) ? 0 : -1;
}

/* CreateWindow (opcode 1): window with background pixel + event mask */
static int x11_create_window(x11_conn_t *c, uint32_t wid,
                              uint32_t parent,
                              int16_t x, int16_t y,
                              uint16_t w, uint16_t h,
                              uint32_t bg_pixel,
                              uint32_t event_mask) {
    /* value_mask = CWBackPixel(0x02) | CWEventMask(0x800) = 0x0802
     * 2 value words → total length = (32 + 8) / 4 = 10 dwords */
    uint8_t req[40];
    req[0] = 1;                     /* opcode: CreateWindow */
    req[1] = c->root_depth;         /* depth */
    put_le16(req + 2, 10);          /* length */
    put_le32(req + 4, wid);
    put_le32(req + 8, parent);
    put_le16(req + 12, (uint16_t)x);
    put_le16(req + 14, (uint16_t)y);
    put_le16(req + 16, w);
    put_le16(req + 18, h);
    put_le16(req + 20, 0);          /* border_width */
    put_le16(req + 22, 1);          /* class: InputOutput */
    put_le32(req + 24, c->root_visual);
    put_le32(req + 28, 0x0802);     /* CWBackPixel | CWEventMask */
    put_le32(req + 32, bg_pixel);   /* background pixel value */
    put_le32(req + 36, event_mask); /* selected events */
    return (write(c->fd, req, 40) == 40) ? 0 : -1;
}

/* MapWindow (opcode 8) */
static int x11_map_window(x11_conn_t *c, uint32_t wid) {
    uint8_t req[8];
    req[0] = 8; req[1] = 0;
    put_le16(req + 2, 2);
    put_le32(req + 4, wid);
    return (write(c->fd, req, 8) == 8) ? 0 : -1;
}

/* PolyFillRectangle (opcode 70): draw one filled rectangle */
static int x11_fill_rect(x11_conn_t *c, uint32_t drawable,
                          uint32_t gc,
                          int16_t x, int16_t y,
                          uint16_t w, uint16_t h) {
    /* 12 bytes header + 8 bytes per rectangle = 20 = 5 dwords */
    uint8_t req[20];
    req[0] = 70; req[1] = 0;
    put_le16(req + 2, 5);
    put_le32(req + 4, drawable);
    put_le32(req + 8, gc);
    put_le16(req + 12, (uint16_t)x);
    put_le16(req + 14, (uint16_t)y);
    put_le16(req + 16, w);
    put_le16(req + 18, h);
    return (write(c->fd, req, 20) == 20) ? 0 : -1;
}

/* DestroyWindow (opcode 4) */
static int x11_destroy_window(x11_conn_t *c, uint32_t wid) {
    uint8_t req[8];
    req[0] = 4; req[1] = 0;
    put_le16(req + 2, 2);
    put_le32(req + 4, wid);
    return (write(c->fd, req, 8) == 8) ? 0 : -1;
}

/* Read one X11 event (32 bytes).  Handles variable-length replies and
 * GenericEvents by consuming their extra data transparently.
 * Returns the event code (2-34), 0 for Error, or -1 for read failure. */
static int x11_read_event(int fd, uint8_t ev[32]) {
    if (read_exact(fd, ev, 32) < 0) return -1;
    uint8_t type = ev[0] & 0x7F;

    /* Reply (type 1) or GenericEvent (type 35) have extra data */
    if (type == 1 || type == 35) {
        uint32_t extra = le32(ev + 4) * 4;
        uint8_t tmp[256];
        while (extra > 0) {
            int chunk = extra > 256 ? 256 : (int)extra;
            if (read_exact(fd, tmp, chunk) < 0) return -1;
            extra -= (uint32_t)chunk;
        }
        return (type == 1) ? -2 : 35; /* -2 = reply (skip) */
    }
    return type;
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    int passes = 0, fails = 0;

    printf("test-x11: X11 raw protocol tests\n");

    /* ── DISPLAY parsing tests (always run, no X server needed) ──── */

    TEST("DISPLAY parse ':0'");
    {
        display_info_t d;
        if (parse_display(":0", &d) == 0 &&
            d.type == DISPLAY_UNIX &&
            d.display_number == 0 &&
            strcmp(d.socket_path, "/tmp/.X11-unix/X0") == 0)
            PASS();
        else FAIL("wrong parse");
    }

    TEST("DISPLAY parse ':1.2'");
    {
        display_info_t d;
        if (parse_display(":1.2", &d) == 0 &&
            d.display_number == 1 && d.screen_number == 2)
            PASS();
        else FAIL("wrong parse");
    }

    TEST("DISPLAY parse 'localhost:0'");
    {
        display_info_t d;
        if (parse_display("localhost:0", &d) == 0 &&
            d.type == DISPLAY_TCP &&
            strcmp(d.hostname, "localhost") == 0 &&
            d.display_number == 0)
            PASS();
        else FAIL("wrong parse");
    }

    TEST("DISPLAY parse '/path/sock:0'");
    {
        display_info_t d;
        /* Path doesn't exist on disk — parse_display falls back to
         * the full string as socket_path */
        if (parse_display("/path/to/sock:0", &d) == 0 &&
            d.type == DISPLAY_UNIX && d.display_number == 0)
            PASS();
        else FAIL("wrong parse");
    }

    TEST("DISPLAY parse NULL/empty");
    {
        display_info_t d;
        if (parse_display(NULL, &d) < 0 &&
            parse_display("", &d) < 0)
            PASS();
        else FAIL("should reject");
    }

    /* ── X server tests (skip if DISPLAY is unset) ───────────────── */

    const char *display_env = getenv("DISPLAY");
    if (!display_env || !*display_env) {
        TEST("X11 socket connect");  SKIP("DISPLAY not set");
        TEST("X11 window + draw");   SKIP("DISPLAY not set");
        SUMMARY("test-x11");
        return fails > 0 ? 1 : 0;
    }

    display_info_t dinfo;
    if (parse_display(display_env, &dinfo) < 0) {
        TEST("X11 socket connect");  FAIL("cannot parse DISPLAY");
        SUMMARY("test-x11");
        return fails > 0 ? 1 : 0;
    }

    /* Try to find Xauthority for authentication */
    xauth_t auth;
    int have_auth = (find_xauth(dinfo.display_number, &auth) == 0);

    /* Connect to X server */
    TEST("X11 socket connect");
    x11_conn_t conn;
    int connected = (x11_connect(&dinfo, have_auth ? &auth : NULL,
                                 &conn) == 0);
    /* If auth failed, retry without auth (some servers allow it) */
    if (!connected && have_auth)
        connected = (x11_connect(&dinfo, NULL, &conn) == 0);

    if (!connected) {
        SKIP("cannot connect to X server");
        TEST("X11 window + draw");  SKIP("no connection");
        SUMMARY("test-x11");
        return fails > 0 ? 1 : 0;
    }
    printf("OK (root=0x%x depth=%d)\n", conn.root_window,
           conn.root_depth);
    passes++;

    /* Create window, map it, wait for Expose, draw a rectangle */
    TEST("X11 window + draw");
    {
        uint32_t wid  = x11_alloc_id(&conn);
        uint32_t gcid = x11_alloc_id(&conn);

        /* Event mask: ExposureMask(15) | KeyPressMask(0) */
        uint32_t events = (1u << 15) | (1u << 0);
        int ok = 0;

        /* Use white_pixel/black_pixel for maximum visual compatibility
         * (works on TrueColor, PseudoColor, and any other visual) */
        if (x11_create_window(&conn, wid, conn.root_window,
                               100, 100, 200, 200,
                               conn.white_pixel, events) < 0 ||
            x11_create_gc(&conn, gcid, wid, conn.black_pixel) < 0 ||
            x11_map_window(&conn, wid) < 0) {
            FAIL("request write failed");
        } else {
            /* Poll for Expose event (up to ~5 seconds) */
            for (int i = 0; i < 50 && !ok; i++) {
                struct pollfd pfd = { .fd = conn.fd, .events = POLLIN };
                if (poll(&pfd, 1, 100) <= 0) continue;

                uint8_t ev[32];
                int type = x11_read_event(conn.fd, ev);
                if (type < 0 || type == 0) break; /* error or X error */
                if (type == -2) continue;          /* reply — skip */

                if (type == 12) { /* Expose */
                    x11_fill_rect(&conn, wid, gcid, 40, 40, 120, 120);
                    ok = 1;
                }
            }
            x11_destroy_window(&conn, wid);
            if (ok) PASS();
            else FAIL("no Expose event within 5s");
        }
    }

    close(conn.fd);

    SUMMARY("test-x11");
    return fails > 0 ? 1 : 0;
}
