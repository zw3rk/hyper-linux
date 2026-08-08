/* syscall_stats.c — Guest-syscall volume profiling
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "syscall_stats.h"
#include "syscall.h"   /* SYS_* numbers */
#include "syscall_internal.h" /* fd_table, FD_TABLE_SIZE, FD_SOCKET */
#include "trace.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

/* ---------- State ---------- */

static int stats_on;
static atomic_ullong counts[HL_SYS_STATS_MAX_NR];
static atomic_ullong total_syscalls;
static atomic_ullong bytes_read;
static atomic_ullong bytes_write;
static atomic_ullong bytes_read_sock;
static atomic_ullong bytes_write_sock;
static atomic_ullong bytes_read_x11;
static atomic_ullong bytes_write_x11;
static atomic_ullong calls_read_x11;
static atomic_ullong calls_write_x11;
static atomic_ullong calls_read_sock;
static atomic_ullong calls_write_sock;
static atomic_ullong poll_calls;       /* poll+ppoll+pselect6+epoll_pwait */
static atomic_ullong poll_zero_ret;    /* ret==0 (timeout/idle wake) */
static atomic_ullong eagain_total;     /* result == -LINUX_EAGAIN (any nr) */
static atomic_ullong eagain_recv;      /* recvmsg/recvfrom/read/readv */
static atomic_ullong eagain_send;      /* writev/write/sendto/sendmsg */
static atomic_ullong eagain_poll;      /* rare; poll family returning EAGAIN */
static atomic_ullong ns_in_dispatch;   /* optional wall time in handlers */
static int time_handlers;             /* HL_SYSCALL_STATS=time */

/* Guest FD → X11 display connection. Fixed table; races are benign for stats. */
#define X11_FD_SLOTS 128
static atomic_int x11_fd[X11_FD_SLOTS];

static struct timespec t0;
static pthread_mutex_t dump_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Names for common hot syscalls ---------- */

static const char *sys_name(unsigned nr) {
    switch (nr) {
    case SYS_write: return "write";
    case SYS_read: return "read";
    case SYS_writev: return "writev";
    case SYS_readv: return "readv";
    case SYS_close: return "close";
    case SYS_openat: return "openat";
    case SYS_ioctl: return "ioctl";
    case SYS_fcntl: return "fcntl";
    case SYS_ppoll: return "ppoll";
    case SYS_pselect6: return "pselect6";
    case SYS_epoll_pwait: return "epoll_pwait";
    case SYS_epoll_ctl: return "epoll_ctl";
    case SYS_epoll_create1: return "epoll_create1";
    case SYS_recvfrom: return "recvfrom";
    case SYS_sendto: return "sendto";
    case SYS_recvmsg: return "recvmsg";
    case SYS_sendmsg: return "sendmsg";
    case SYS_socket: return "socket";
    case SYS_connect: return "connect";
    case SYS_bind: return "bind";
    case SYS_listen: return "listen";
    case SYS_accept: return "accept";
    case SYS_accept4: return "accept4";
    case SYS_clock_gettime: return "clock_gettime";
    case SYS_gettimeofday: return "gettimeofday";
    case SYS_nanosleep: return "nanosleep";
    case SYS_futex: return "futex";
    case SYS_clone: return "clone";
    case SYS_mmap: return "mmap";
    case SYS_munmap: return "munmap";
    case SYS_mprotect: return "mprotect";
    case SYS_rt_sigprocmask: return "rt_sigprocmask";
    case SYS_rt_sigaction: return "rt_sigaction";
    case SYS_getpid: return "getpid";
    case SYS_gettid: return "gettid";
    case SYS_brk: return "brk";
    case SYS_lseek: return "lseek";
    case SYS_newfstatat: return "newfstatat";
    case SYS_fstat: return "fstat";
    case SYS_getdents64: return "getdents64";
    case SYS_exit_group: return "exit_group";
    case SYS_exit: return "exit";
    default: return NULL;
    }
}

/* ---------- Public API ---------- */

void syscall_stats_init(void) {
    if (stats_on) return;

    const char *env = getenv("HL_SYSCALL_STATS");
    int want = 0;
    if (env && env[0] && env[0] != '0' && env[0] != 'n' && env[0] != 'N') {
        want = 1;
        if (strcmp(env, "time") == 0 || strstr(env, "time") != NULL)
            time_handlers = 1;
    }
    /* HL_TRACE=sys also enables (bit set by trace init before/after). */
    if (hl_trace_on(HL_TRACE_SYS))
        want = 1;

    if (!want) return;

    stats_on = 1;
    for (int i = 0; i < X11_FD_SLOTS; i++)
        atomic_store(&x11_fd[i], 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    fprintf(stderr, "hl: syscall_stats enabled%s (SIGUSR1 dumps; atexit dumps)\n",
            time_handlers ? " +handler_time" : "");
    fflush(stderr);
}

int syscall_stats_enabled(void) {
    return stats_on;
}

void syscall_stats_mark_x11_fd(int guest_fd) {
    if (!stats_on || guest_fd < 0 || guest_fd >= X11_FD_SLOTS) return;
    atomic_store(&x11_fd[guest_fd], 1);
    if (hl_trace_on(HL_TRACE_SYS))
        hl_trace(HL_TRACE_SYS, "x11_fd gfd=%d", guest_fd);
}

void syscall_stats_clear_fd(int guest_fd) {
    if (!stats_on || guest_fd < 0 || guest_fd >= X11_FD_SLOTS) return;
    atomic_store(&x11_fd[guest_fd], 0);
}

static int is_x11_fd(int gfd) {
    if (gfd < 0 || gfd >= X11_FD_SLOTS) return 0;
    return atomic_load(&x11_fd[gfd]) != 0;
}

static void stats_service_dump_request(void);

void syscall_stats_note(unsigned nr, int64_t result,
                        uint64_t a0, uint64_t a1, uint64_t a2) {
    stats_service_dump_request();
    if (!stats_on) return;

    if (nr < HL_SYS_STATS_MAX_NR)
        atomic_fetch_add(&counts[nr], 1);
    atomic_fetch_add(&total_syscalls, 1);

    /* Byte / socket / X11 attribution for stream I/O */
    if (result > 0) {
        int gfd = (int)a0;
        int x11 = is_x11_fd(gfd);
        int is_sock = 0;
        if (gfd >= 0 && gfd < FD_TABLE_SIZE &&
            fd_table[gfd].type == FD_SOCKET)
            is_sock = 1;
        switch ((int)nr) {
        case SYS_read:
        case SYS_readv:
        case SYS_recvfrom:
        case SYS_recvmsg:
            atomic_fetch_add(&bytes_read, (unsigned long long)result);
            if (is_sock || x11) {
                atomic_fetch_add(&bytes_read_sock, (unsigned long long)result);
                atomic_fetch_add(&calls_read_sock, 1);
            }
            if (x11) {
                atomic_fetch_add(&bytes_read_x11, (unsigned long long)result);
                atomic_fetch_add(&calls_read_x11, 1);
            }
            break;
        case SYS_write:
        case SYS_writev:
        case SYS_sendto:
        case SYS_sendmsg:
            atomic_fetch_add(&bytes_write, (unsigned long long)result);
            if (is_sock || x11) {
                atomic_fetch_add(&bytes_write_sock, (unsigned long long)result);
                atomic_fetch_add(&calls_write_sock, 1);
            }
            if (x11) {
                atomic_fetch_add(&bytes_write_x11, (unsigned long long)result);
                atomic_fetch_add(&calls_write_x11, 1);
            }
            break;
        default:
            break;
        }
        (void)a1;
        (void)a2;
    }

    switch ((int)nr) {
    case SYS_ppoll:
    case SYS_pselect6:
    case SYS_epoll_pwait:
        atomic_fetch_add(&poll_calls, 1);
        if (result == 0)
            atomic_fetch_add(&poll_zero_ret, 1);
        break;
    default:
        break;
    }

    /* EAGAIN buckets — next-bottleneck signal for socket/X fast paths */
    if (result == -LINUX_EAGAIN) {
        atomic_fetch_add(&eagain_total, 1);
        switch ((int)nr) {
        case SYS_read:
        case SYS_readv:
        case SYS_recvfrom:
        case SYS_recvmsg:
            atomic_fetch_add(&eagain_recv, 1);
            break;
        case SYS_write:
        case SYS_writev:
        case SYS_sendto:
        case SYS_sendmsg:
            atomic_fetch_add(&eagain_send, 1);
            break;
        case SYS_ppoll:
        case SYS_pselect6:
        case SYS_epoll_pwait:
            atomic_fetch_add(&eagain_poll, 1);
            break;
        default:
            break;
        }
    }
}

void syscall_stats_note_time_ns(unsigned long long ns) {
    if (!stats_on || !time_handlers) return;
    atomic_fetch_add(&ns_in_dispatch, ns);
}

/* Exposed for dispatch timing wrapper */
int syscall_stats_time_handlers(void) {
    return stats_on && time_handlers;
}

void syscall_stats_reset(void) {
    if (!stats_on) return;
    for (int i = 0; i < HL_SYS_STATS_MAX_NR; i++)
        atomic_store(&counts[i], 0);
    atomic_store(&total_syscalls, 0);
    atomic_store(&bytes_read, 0);
    atomic_store(&bytes_write, 0);
    atomic_store(&bytes_read_sock, 0);
    atomic_store(&bytes_write_sock, 0);
    atomic_store(&bytes_read_x11, 0);
    atomic_store(&bytes_write_x11, 0);
    atomic_store(&calls_read_x11, 0);
    atomic_store(&calls_write_x11, 0);
    atomic_store(&calls_read_sock, 0);
    atomic_store(&calls_write_sock, 0);
    atomic_store(&poll_calls, 0);
    atomic_store(&poll_zero_ret, 0);
    atomic_store(&eagain_total, 0);
    atomic_store(&eagain_recv, 0);
    atomic_store(&eagain_send, 0);
    atomic_store(&eagain_poll, 0);
    atomic_store(&ns_in_dispatch, 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
}

static int cmp_pair(const void *a, const void *b) {
    const struct { unsigned nr; unsigned long long c; } *pa = a, *pb = b;
    if (pa->c < pb->c) return 1;
    if (pa->c > pb->c) return -1;
    return (int)pa->nr - (int)pb->nr;
}

void syscall_stats_dump_fp(FILE *fp, const char *label) {
    if (!stats_on || !fp) return;

    pthread_mutex_lock(&dump_lock);

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (double)(t1.tv_sec - t0.tv_sec) +
                 (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (sec < 1e-6) sec = 1e-6;

    unsigned long long total = atomic_load(&total_syscalls);
    unsigned long long br = atomic_load(&bytes_read);
    unsigned long long bw = atomic_load(&bytes_write);
    unsigned long long brs = atomic_load(&bytes_read_sock);
    unsigned long long bws = atomic_load(&bytes_write_sock);
    unsigned long long crs = atomic_load(&calls_read_sock);
    unsigned long long cws = atomic_load(&calls_write_sock);
    unsigned long long brx = atomic_load(&bytes_read_x11);
    unsigned long long bwx = atomic_load(&bytes_write_x11);
    unsigned long long crx = atomic_load(&calls_read_x11);
    unsigned long long cwx = atomic_load(&calls_write_x11);
    unsigned long long pc = atomic_load(&poll_calls);
    unsigned long long pz = atomic_load(&poll_zero_ret);
    unsigned long long eag = atomic_load(&eagain_total);
    unsigned long long eag_r = atomic_load(&eagain_recv);
    unsigned long long eag_s = atomic_load(&eagain_send);
    unsigned long long eag_p = atomic_load(&eagain_poll);
    unsigned long long nsd = atomic_load(&ns_in_dispatch);

    fprintf(fp, "\n======== hl syscall_stats%s%s ========\n",
            label && label[0] ? ": " : "",
            label && label[0] ? label : "");
    fprintf(fp, "window_s=%.3f  total_syscalls=%llu  rate=%.0f/s\n",
            sec, total, (double)total / sec);
    fprintf(fp, "io_bytes read=%llu write=%llu  (%.1f + %.1f KiB/s)\n",
            br, bw, (br / 1024.0) / sec, (bw / 1024.0) / sec);
    fprintf(fp, "sock_io read=%lluB/%llu calls  write=%lluB/%llu calls"
            "  (%.1f + %.1f KiB/s)\n",
            brs, crs, bws, cws,
            (brs / 1024.0) / sec, (bws / 1024.0) / sec);
    fprintf(fp, "x11_io  read=%lluB/%llu calls  write=%lluB/%llu calls"
            "  (%.1f + %.1f KiB/s)\n",
            brx, crx, bwx, cwx,
            (brx / 1024.0) / sec, (bwx / 1024.0) / sec);
    fprintf(fp, "poll_family calls=%llu zero_ret=%llu (%.1f%% idle/timeout)"
            " rate=%.0f/s\n",
            pc, pz, pc ? (100.0 * (double)pz / (double)pc) : 0.0,
            (double)pc / sec);
    fprintf(fp, "eagain total=%llu (%.0f/s)  recv=%llu send=%llu poll=%llu\n",
            eag, (double)eag / sec, eag_r, eag_s, eag_p);
    if (time_handlers && nsd)
        fprintf(fp, "handler_time_ms=%.1f  (%.1f%% of wall if single-threaded)\n",
                nsd / 1e6, 100.0 * ((double)nsd / 1e9) / sec);

    /* Top-N by count */
    struct { unsigned nr; unsigned long long c; } top[HL_SYS_STATS_MAX_NR];
    int ntop = 0;
    for (unsigned i = 0; i < HL_SYS_STATS_MAX_NR; i++) {
        unsigned long long c = atomic_load(&counts[i]);
        if (c == 0) continue;
        top[ntop].nr = i;
        top[ntop].c = c;
        ntop++;
    }
    qsort(top, (size_t)ntop, sizeof(top[0]), cmp_pair);

    int show = ntop < 25 ? ntop : 25;
    fprintf(fp, "top_syscalls (count, rate/s, %%):\n");
    for (int i = 0; i < show; i++) {
        const char *name = sys_name(top[i].nr);
        double rate = (double)top[i].c / sec;
        double pct = total ? 100.0 * (double)top[i].c / (double)total : 0.0;
        if (name)
            fprintf(fp, "  %5u %-18s %10llu  %8.0f/s  %5.1f%%\n",
                    top[i].nr, name, top[i].c, rate, pct);
        else
            fprintf(fp, "  %5u %-18s %10llu  %8.0f/s  %5.1f%%\n",
                    top[i].nr, "?", top[i].c, rate, pct);
    }
    fprintf(fp, "======== end syscall_stats ========\n");
    fflush(fp);

    pthread_mutex_unlock(&dump_lock);
}

void syscall_stats_dump(const char *label) {
    syscall_stats_dump_fp(stderr, label);
}

static _Atomic int stats_dump_requested;

/* Async-signal-safe: just latch a request. The previous handler called
 * pthread_mutex_lock, fprintf, qsort and getenv directly — none of which is
 * async-signal-safe — and would self-deadlock on dump_lock if the signal
 * landed on the thread already holding it (e.g. during the atexit dump). */
static void on_sigusr1(int sig) {
    (void)sig;
    atomic_store(&stats_dump_requested, 1);
}

/* Called from the syscall path, outside signal context. */
static void stats_service_dump_request(void) {
    if (!atomic_load(&stats_dump_requested)) return;
    atomic_store(&stats_dump_requested, 0);
    syscall_stats_dump("SIGUSR1");
    const char *r = getenv("HL_SYSCALL_STATS_RESET");
    if (r && (*r == '1' || *r == 'y' || *r == 'Y'))
        syscall_stats_reset();
}

/* Service point of last resort.
 *
 * stats_service_dump_request() is otherwise only reached from the syscall
 * path, so a SIGUSR1 that arrives while the guest is making no syscalls —
 * parked in an indefinite poll or futex, or busy in pure computation —
 * latched a request that was never serviced. The dump appeared minutes
 * later, when the guest happened to make a syscall, or not at all.
 *
 * A plain thread is the right context: the handler stays async-signal-safe
 * (it only sets a flag) and the actual dump runs with locks and stdio
 * available, exactly as it does on the syscall path. */
static void *stats_service_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        stats_service_dump_request();
    }
    return NULL;
}

void syscall_stats_install_signal(void) {
    if (!stats_on) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;
    sa.sa_flags = SA_RESTART;   /* do not surface EINTR to in-flight I/O */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, stats_service_thread, NULL) == 0)
        pthread_detach(th);
}

static void atexit_dump(void) {
    if (stats_on)
        syscall_stats_dump("atexit");
}

void syscall_stats_atexit_register(void) {
    if (stats_on)
        atexit(atexit_dump);
}
