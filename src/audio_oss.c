/* audio_oss.c — Linux OSS /dev/dsp + /dev/mixer
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_oss.h"
#include "audio.h"
#include <sys/types.h>
#include "linux_oss_abi.h"
#include "device.h"
#include "fd_object.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "guest.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>

/* Forward: ops defined below; fork_import needs the pointer. */
static const hl_fd_ops_t oss_dsp_ops;

typedef struct oss_dsp_state {
    hl_audio_stream_t *stream;
    int format;    /* AFMT_* */
    int channels;
    int speed;
    int frag_arg;  /* SETFRAGMENT packed */
    uint32_t rdev; /* Linux-encoded dev of the node this fd was opened from */
} oss_dsp_state_t;

typedef struct oss_mixer_state {
    int vol_left, vol_right; /* 0..100 */
    int pcm_left, pcm_right;
} oss_mixer_state_t;

/* Global mixer levels applied to new DSP streams */
static oss_mixer_state_t g_mixer = { 100, 100, 100, 100 };

/*
 * Live DSP streams so /dev/mixer WRITE_* can push software gain while
 * audio is already open. Without this, XMMS volume slider updates only
 * g_mixer and leaves the playing stream at 100/100 forever.
 */
#define OSS_LIVE_DSP_MAX 64
static hl_audio_stream_t *g_live_dsp[OSS_LIVE_DSP_MAX];
static int g_live_dsp_n;
static pthread_mutex_t g_live_dsp_lock = PTHREAD_MUTEX_INITIALIZER;

static void
oss_effective_vol(int *left, int *right)
{
    int l = (g_mixer.vol_left * g_mixer.pcm_left) / 100;
    int r = (g_mixer.vol_right * g_mixer.pcm_right) / 100;
    if (l < 0) l = 0;
    if (l > 100) l = 100;
    if (r < 0) r = 0;
    if (r > 100) r = 100;
    if (left) *left = l;
    if (right) *right = r;
}

static void
oss_live_dsp_add(hl_audio_stream_t *s)
{
    if (!s) return;
    pthread_mutex_lock(&g_live_dsp_lock);
    if (g_live_dsp_n < OSS_LIVE_DSP_MAX)
        g_live_dsp[g_live_dsp_n++] = s;
    pthread_mutex_unlock(&g_live_dsp_lock);
}

static void
oss_live_dsp_del(hl_audio_stream_t *s)
{
    int i;
    if (!s) return;
    pthread_mutex_lock(&g_live_dsp_lock);
    for (i = 0; i < g_live_dsp_n; i++) {
        if (g_live_dsp[i] == s) {
            g_live_dsp[i] = g_live_dsp[--g_live_dsp_n];
            break;
        }
    }
    pthread_mutex_unlock(&g_live_dsp_lock);
}

static void
oss_push_mixer_to_streams(void)
{
    int i, l, r;
    oss_effective_vol(&l, &r);
    pthread_mutex_lock(&g_live_dsp_lock);
    for (i = 0; i < g_live_dsp_n; i++)
        hl_audio_stream_set_volume(g_live_dsp[i], l, r);
    pthread_mutex_unlock(&g_live_dsp_lock);
    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO,
                 "mixer push n=%d vol=%d/%d pcm=%d/%d effective=%d/%d",
                 g_live_dsp_n, g_mixer.vol_left, g_mixer.vol_right,
                 g_mixer.pcm_left, g_mixer.pcm_right, l, r);
}

static int64_t oss_write(hl_open_file_t *of, int host_fd,
                         guest_t *g, uint64_t buf_gva, uint64_t count) {
    (void)host_fd;
    oss_dsp_state_t *st = of->state;
    if (!st || !st->stream) return -LINUX_ENODEV;
    if (count > 1 << 20) count = 1 << 20;
    void *tmp = malloc((size_t)count);
    if (!tmp) return -LINUX_ENOMEM;
    if (guest_read(g, buf_gva, tmp, (size_t)count) < 0) {
        free(tmp);
        return -LINUX_EFAULT;
    }
    st->stream->nonblock =
        (atomic_load(&of->status_flags) & LINUX_O_NONBLOCK) != 0;
    int64_t w = hl_audio_stream_write(st->stream, tmp, (size_t)count);
    free(tmp);
    if (w < 0) {
        if (errno == EAGAIN) return -LINUX_EAGAIN;
        return -LINUX_EIO;
    }
    return w;
}

static int64_t oss_read(hl_open_file_t *of, int host_fd,
                        guest_t *g, uint64_t buf_gva, uint64_t count) {
    (void)of; (void)host_fd; (void)g; (void)buf_gva; (void)count;
    return -LINUX_ENXIO; /* capture not supported */
}

static int pack_vol(int l, int r) {
    if (l < 0) l = 0;
    if (l > 100) l = 100;
    if (r < 0) r = 0;
    if (r > 100) r = 100;
    return (r << 8) | l;
}

static void unpack_vol(int v, int *l, int *r) {
    *l = v & 0xff;
    *r = (v >> 8) & 0xff;
    if (*l > 100) *l = 100;
    if (*r > 100) *r = 100;
}

static int64_t oss_ioctl(hl_open_file_t *of, int host_fd,
                         guest_t *g, uint64_t request, uint64_t arg_gva) {
    (void)host_fd;
    oss_dsp_state_t *st = of->state;
    if (!st || !st->stream) return -LINUX_ENODEV;
    hl_audio_stream_t *s = st->stream;
    uint32_t req = (uint32_t)request;

    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO, "oss ioctl req=0x%x stream=%llu",
                 req, (unsigned long long)s->id);

    if (req == SNDCTL_DSP_RESET) {
        hl_audio_stream_reset(s);
        return 0;
    }
    if (req == SNDCTL_DSP_POST) {
        hl_audio_stream_post(s);
        return 0;
    }
    if (req == SNDCTL_DSP_SYNC) {
        /* OSS contract: SYNC drains, RESET drops. Both used to alias POST,
         * so SYNC returned without waiting for playback to finish. Report a
         * drain timeout to the guest (EIO) instead of swallowing it (V9). */
        hl_audio_stream_post(s);
        if (hl_audio_stream_drain(s) < 0)
            return -LINUX_EIO;
        return 0;
    }
    if (req == SNDCTL_DSP_NONBLOCK) {
        atomic_fetch_or(&of->status_flags, LINUX_O_NONBLOCK);
        s->nonblock = 1;
        return 0;
    }

    int ival = 0;
    if (arg_gva && (req != SNDCTL_DSP_RESET && req != SNDCTL_DSP_POST &&
                    req != SNDCTL_DSP_SYNC && req != SNDCTL_DSP_NONBLOCK)) {
        if (guest_read(g, arg_gva, &ival, sizeof(ival)) < 0)
            return -LINUX_EFAULT;
    }

    if (req == SNDCTL_DSP_SETFMT) {
        if (ival == AFMT_QUERY) {
            ival = st->format ? st->format : AFMT_S16_LE;
        } else if (ival == AFMT_U8 || ival == AFMT_S16_LE) {
            st->format = ival;
        } else {
            /* fallback */
            st->format = AFMT_S16_LE;
            ival = AFMT_S16_LE;
        }
        hl_audio_params_t p = s->params;
        p.format = (st->format == AFMT_U8) ? HL_AUDIO_FMT_U8 : HL_AUDIO_FMT_S16_LE;
        hl_audio_stream_configure(s, &p);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_GETFMTS) {
        ival = AFMT_U8 | AFMT_S16_LE;
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_STEREO) {
        st->channels = ival ? 2 : 1;
        ival = st->channels == 2 ? 1 : 0;
        hl_audio_params_t p = s->params;
        p.channels = st->channels;
        hl_audio_stream_configure(s, &p);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_CHANNELS) {
        if (ival < 1) ival = 1;
        if (ival > 2) ival = 2;
        st->channels = ival;
        hl_audio_params_t p = s->params;
        p.channels = st->channels;
        hl_audio_stream_configure(s, &p);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_SPEED) {
        if (ival < 4000) ival = 4000;
        if (ival > 192000) ival = 192000;
        st->speed = ival;
        hl_audio_params_t p = s->params;
        p.rate = st->speed;
        hl_audio_stream_configure(s, &p);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_SETFRAGMENT) {
        st->frag_arg = ival;
        /* Clamp the shift before applying: `1 << n` is undefined for
         * n >= 32 (C11 6.5.7p3), and the guest supplies n directly. */
        int frag_shift = ival & 0xffff;
        if (frag_shift < 8)  frag_shift = 8;
        if (frag_shift > 16) frag_shift = 16;
        int frag_size = 1 << frag_shift;
        int frag_count = (ival >> 16) & 0xffff;
        if (frag_size < 256) frag_size = 256;
        if (frag_size > 65536) frag_size = 65536;
        if (frag_count < 2) frag_count = 2;
        if (frag_count > 128) frag_count = 128;
        /*
         * hl: small OSS fragments (e.g. 256×4) cause guest write/pselect/ioctl
         * storms under HVF (~180 pselect/s + ~100 ioctl/s while XMMS plays).
         * Floor size/count so the app still works but with fewer wakeups.
         * Return the *requested* ival in the ioctl out-arg (common OSS apps
         * ignore the echo); stream uses the floored params.
         */
        if (frag_size < 4096) frag_size = 4096;
        if (frag_count < 16) frag_count = 16;
        hl_audio_params_t p = s->params;
        p.frag_size = frag_size;
        p.frag_count = frag_count;
        hl_audio_stream_configure(s, &p);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_GETBLKSIZE) {
        ival = s->params.frag_size;
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_GETOSPACE) {
        hl_audio_space_t sp;
        hl_audio_stream_get_space(s, &sp);
        audio_buf_info info;
        info.fragsize = s->params.frag_size;
        info.fragstotal = s->params.frag_count;
        info.bytes = (int)sp.free_bytes;
        info.fragments = info.fragsize
            ? (int)(sp.free_bytes / (uint64_t)info.fragsize) : 0;
        if (guest_write(g, arg_gva, &info, sizeof(info)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_GETODELAY) {
        hl_audio_space_t sp;
        hl_audio_stream_get_space(s, &sp);
        ival = (int)sp.pending;
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_GETCAPS) {
        ival = 0; /* basic */
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SNDCTL_DSP_SETPLAYVOL || req == SNDCTL_DSP_GETPLAYVOL) {
        if (req == SNDCTL_DSP_SETPLAYVOL) {
            int l, r;
            unpack_vol(ival, &l, &r);
            hl_audio_stream_set_volume(s, l, r);
        }
        int l, r;
        hl_audio_stream_get_volume(s, &l, &r);
        ival = pack_vol(l, r);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }

    /* Mixer-style on DSP: SOUND_MIXER_* */
    if (req == SOUND_MIXER_WRITE_VOLUME || req == SOUND_MIXER_WRITE_PCM) {
        int l, r;
        unpack_vol(ival, &l, &r);
        hl_audio_stream_set_volume(s, l, r);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (req == SOUND_MIXER_READ_VOLUME || req == SOUND_MIXER_READ_PCM) {
        int l, r;
        hl_audio_stream_get_volume(s, &l, &r);
        ival = pack_vol(l, r);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }

    return -LINUX_EINVAL;
}

/* Neutral metadata (mode, rdev) for an OSS fd — like hl_device_fd_stat, but
 * for FD_OSS_DSP/FD_OSS_MIXER. statx must NOT reuse oss_fstat (that writes a
 * linux_stat_t, the wrong layout for statx). Returns 0 if `guest_fd` is an
 * OSS fd, -1 otherwise. */
int hl_oss_fd_stat_meta(int guest_fd, uint32_t *mode_out, uint64_t *rdev_out) {
    if (guest_fd < 0 || guest_fd >= FD_TABLE_SIZE) return -1;
    int type = fd_table[guest_fd].type;
    if (type != FD_OSS_DSP && type != FD_OSS_MIXER) return -1;
    hl_open_file_t *of = fd_table[guest_fd].of;
    if (mode_out) *mode_out = 0020000 | 0666;   /* S_IFCHR | 0666 */
    if (rdev_out) {
        /* macOS makedev() encoding (major<<24 | minor), matching what
         * hl_device_fd_stat returns and what statx/translate_stat decode —
         * NOT the Linux (major<<8|minor) that oss_fstat writes directly. */
        int minor = 3;   /* /dev/dsp default */
        if (type == FD_OSS_DSP) {
            oss_dsp_state_t *dst = of ? of->state : NULL;
            if (dst && dst->rdev) minor = (int)(dst->rdev & 0xff);
        } else {
            minor = 0;   /* /dev/mixer */
        }
        *rdev_out = (uint64_t)makedev(14, minor);
    }
    return 0;
}

static int64_t oss_fstat(hl_open_file_t *of, int host_fd,
                         guest_t *g, uint64_t stat_gva) {
    (void)host_fd;
    linux_stat_t st;
    memset(&st, 0, sizeof(st));
    st.st_mode = 0020000 | 0666;   /* Linux S_IFCHR | 0666 */
    /* Report the same dev the registry (and therefore stat()) uses. This
     * was hardcoded to major 4 minor 3, so stat() and fstat() on the same
     * open fd disagreed and neither matched the OSS major of 14. */
    {
        oss_dsp_state_t *dst = of ? of->state : NULL;
        st.st_rdev = (dst && dst->rdev) ? dst->rdev : (uint32_t)((14 << 8) | 3);
    }
    st.st_blksize = 4096;
    st.st_nlink = 1;
    if (guest_write(g, stat_gva, &st, sizeof(st)) < 0) return -LINUX_EFAULT;
    return 0;
}

static int oss_poll_fd(hl_open_file_t *of, int descriptor_host_fd) {
    (void)descriptor_host_fd;
    oss_dsp_state_t *st = of->state;
    if (!st || !st->stream) return -1;
    return hl_audio_stream_poll_fd(st->stream);
}

static void oss_destroy(hl_open_file_t *of) {
    oss_dsp_state_t *st = of->state;
    if (!st) return;
    if (st->stream) {
        oss_live_dsp_del(st->stream);
        hl_audio_stream_destroy(st->stream);
    }
    free(st);
    of->state = NULL;
}

static int oss_fork_export(hl_open_file_t *of, hl_fork_object_record_t *out) {
    oss_dsp_state_t *st = of->state;
    memset(out, 0, sizeof(*out));
    out->kind = FD_OSS_DSP;
    out->status_flags = atomic_load(&of->status_flags);
    out->object_id = of->object_id;
    /* recreate-empty: only config, no PCM / no AQ pointers */
    struct {
        int format, channels, speed, frag_arg;
        int vol_l, vol_r;
    } p = {
        st ? st->format : AFMT_S16_LE,
        st ? st->channels : 2,
        st ? st->speed : 44100,
        st ? st->frag_arg : 0,
        100, 100
    };
    if (st && st->stream)
        hl_audio_stream_get_volume(st->stream, &p.vol_l, &p.vol_r);
    memcpy(out->payload, &p, sizeof(p));
    out->payload_len = sizeof(p);
    if (hl_trace_on(HL_TRACE_FORK))
        hl_trace(HL_TRACE_FORK, "oss fork_export policy=recreate-empty");
    return 0;
}

static hl_open_file_t *oss_fork_import(const hl_fork_object_record_t *in,
                                       const hl_fork_context_t *ctx) {
    (void)ctx;
    oss_dsp_state_t *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->stream = hl_audio_stream_create();
    if (!st->stream) { free(st); return NULL; }
    st->format = AFMT_S16_LE;
    st->channels = 2;
    st->speed = 44100;
    if (in->payload_len >= sizeof(int) * 6) {
        struct {
            int format, channels, speed, frag_arg;
            int vol_l, vol_r;
        } p;
        memcpy(&p, in->payload, sizeof(p));
        st->format = p.format;
        st->channels = p.channels;
        st->speed = p.speed;
        st->frag_arg = p.frag_arg;
        hl_audio_params_t ap = {
            .format = (st->format == AFMT_U8) ? HL_AUDIO_FMT_U8 : HL_AUDIO_FMT_S16_LE,
            .channels = st->channels,
            .rate = st->speed,
            .frag_size = 4096,
            .frag_count = 16,
        };
        if (st->frag_arg) {
            /* frag_arg is the guest's raw SETFRAGMENT word. Clamp the shift
             * before applying it: a count >= 32 is undefined behaviour, and
             * anything large overflowed the capacity computation downstream.
             * hl_audio_stream_configure() clamps the results as well. */
            int shift = st->frag_arg & 0xffff;
            if (shift < 8)  shift = 8;    /* 256 bytes */
            if (shift > 16) shift = 16;   /* 64 KiB   */
            ap.frag_size = 1 << shift;
            ap.frag_count = (st->frag_arg >> 16) & 0xffff;
        }
        hl_audio_stream_configure(st->stream, &ap);
        hl_audio_stream_set_volume(st->stream, p.vol_l, p.vol_r);
    }
    oss_live_dsp_add(st->stream);
    /* Empty buffers — do not import PCM */
    return hl_open_file_create(FD_OSS_DSP, &oss_dsp_ops, in->status_flags, st);
}

static const hl_fd_ops_t oss_dsp_ops = {
    .read = oss_read,
    .write = oss_write,
    .ioctl = oss_ioctl,
    .fstat = oss_fstat,
    .poll_host_fd = oss_poll_fd,
    .destroy = oss_destroy,
    .fork_export = oss_fork_export,
    .fork_import = oss_fork_import,
};

static int64_t dsp_open(const char *name, int linux_flags, int mode) {
    (void)mode;
    /* Capture (O_RDONLY without write) not supported */
    int acc = linux_flags & O_ACCMODE;
    /* Linux O_ACCMODE uses same 0x3 */
    if (acc == 0 /* O_RDONLY */) {
        return -LINUX_ENXIO;
    }

    oss_dsp_state_t *st = calloc(1, sizeof(*st));
    if (!st) return -LINUX_ENOMEM;
    st->stream = hl_audio_stream_create();
    if (!st->stream) {
        free(st);
        return -LINUX_ENOMEM;
    }
    st->format = AFMT_S16_LE;
    /* Remember which node this fd was opened from so fstat() reports the
     * same device numbers stat() gets from the registry. */
    {
        char devpath[64];
        snprintf(devpath, sizeof(devpath), "/dev/%s", name ? name : "dsp");
        const hl_device_node_t *n = hl_device_lookup(devpath);
        st->rdev = n ? (uint32_t)(((unsigned)n->minor & 0xffu) |
                                  ((unsigned)n->major << 8) |
                                  (((unsigned)n->minor & ~0xffu) << 12))
                     : (uint32_t)((14u << 8) | 3u);
    }
    st->channels = 2;
    st->speed = 44100;
    {
        int l, r;
        oss_effective_vol(&l, &r);
        hl_audio_stream_set_volume(st->stream, l, r);
    }
    oss_live_dsp_add(st->stream);

    uint32_t stflags = (uint32_t)(linux_flags & (LINUX_O_NONBLOCK | 0x3));
    hl_open_file_t *of = hl_open_file_create(FD_OSS_DSP, &oss_dsp_ops,
                                             stflags, st);
    if (!of) {
        oss_live_dsp_del(st->stream);
        hl_audio_stream_destroy(st->stream);
        free(st);
        return -LINUX_ENOMEM;
    }

    int pollfd = hl_audio_stream_poll_fd(st->stream);
    int host_alias = (pollfd >= 0) ? dup(pollfd) : -1;
    uint32_t fdflags = (linux_flags & LINUX_O_CLOEXEC) ? LINUX_O_CLOEXEC : 0;
    int gfd = hl_fd_install(of, host_alias, fdflags);
    if (gfd < 0) return -LINUX_EMFILE;

    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO, "dsp open gfd=%d stream=%llu",
                 gfd, (unsigned long long)st->stream->id);
    return gfd;
}

/* ---- mixer ---- */

/* True if req is either modern Linux _IOC or legacy OSS SIOC encoding. */
static int
mixer_req_is(uint32_t req, uint32_t modern, uint32_t legacy)
{
    return req == modern || req == legacy;
}

static int64_t mixer_ioctl(hl_open_file_t *of, int host_fd,
                           guest_t *g, uint64_t request, uint64_t arg_gva) {
    (void)of; (void)host_fd;
    uint32_t req = (uint32_t)request;
    int ival = 0;
    if (arg_gva) {
        if (guest_read(g, arg_gva, &ival, sizeof(ival)) < 0)
            return -LINUX_EFAULT;
    }
    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO, "mixer ioctl req=0x%x arg=0x%x", req,
                 (unsigned)ival);

    if (mixer_req_is(req, SOUND_MIXER_READ_DEVMASK,
                     SOUND_MIXER_READ_DEVMASK_LEGACY)) {
        ival = SOUND_MASK_VOLUME | SOUND_MASK_PCM;
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (mixer_req_is(req, SOUND_MIXER_READ_STEREODEVS,
                     SOUND_MIXER_READ_STEREODEVS_LEGACY)) {
        ival = SOUND_MASK_VOLUME | SOUND_MASK_PCM;
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (mixer_req_is(req, SOUND_MIXER_READ_RECMASK,
                     SOUND_MIXER_READ_RECMASK_LEGACY)) {
        ival = 0;
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (mixer_req_is(req, SOUND_MIXER_WRITE_VOLUME,
                     SOUND_MIXER_WRITE_VOLUME_LEGACY)) {
        unpack_vol(ival, &g_mixer.vol_left, &g_mixer.vol_right);
        /* software only — do not touch macOS system volume */
        oss_push_mixer_to_streams();
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (mixer_req_is(req, SOUND_MIXER_READ_VOLUME,
                     SOUND_MIXER_READ_VOLUME_LEGACY)) {
        ival = pack_vol(g_mixer.vol_left, g_mixer.vol_right);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (mixer_req_is(req, SOUND_MIXER_WRITE_PCM,
                     SOUND_MIXER_WRITE_PCM_LEGACY)) {
        unpack_vol(ival, &g_mixer.pcm_left, &g_mixer.pcm_right);
        oss_push_mixer_to_streams();
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (mixer_req_is(req, SOUND_MIXER_READ_PCM,
                     SOUND_MIXER_READ_PCM_LEGACY)) {
        ival = pack_vol(g_mixer.pcm_left, g_mixer.pcm_right);
        if (guest_write(g, arg_gva, &ival, sizeof(ival)) < 0) return -LINUX_EFAULT;
        return 0;
    }
    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO, "mixer ioctl ENOTTY req=0x%x", req);
    return -LINUX_EINVAL;
}

static void mixer_destroy(hl_open_file_t *of) {
    free(of->state);
    of->state = NULL;
}

static int64_t mixer_fstat(hl_open_file_t *of, int host_fd,
                           guest_t *g, uint64_t stat_gva) {
    (void)of; (void)host_fd;
    linux_stat_t st;
    memset(&st, 0, sizeof(st));
    st.st_mode = 0020000 | 0666;
    st.st_rdev = (14 << 8) | 0;
    st.st_nlink = 1;
    if (guest_write(g, stat_gva, &st, sizeof(st)) < 0) return -LINUX_EFAULT;
    return 0;
}

static const hl_fd_ops_t oss_mixer_ops = {
    .ioctl = mixer_ioctl,
    .fstat = mixer_fstat,
    .destroy = mixer_destroy,
};

static int64_t mixer_open(const char *name, int linux_flags, int mode) {
    (void)name; (void)mode;
    oss_mixer_state_t *st = calloc(1, sizeof(*st));
    if (!st) return -LINUX_ENOMEM;
    *st = g_mixer;
    uint32_t stflags = (uint32_t)(linux_flags & LINUX_O_NONBLOCK);
    hl_open_file_t *of = hl_open_file_create(FD_OSS_MIXER, &oss_mixer_ops,
                                             stflags, st);
    if (!of) { free(st); return -LINUX_ENOMEM; }
    uint32_t fdflags = (linux_flags & LINUX_O_CLOEXEC) ? LINUX_O_CLOEXEC : 0;
    int gfd = hl_fd_install(of, -1, fdflags);
    if (gfd < 0) return -LINUX_EMFILE;
    return gfd;
}

static const hl_device_ops_t ops_dsp = { .open = dsp_open };
static const hl_device_ops_t ops_mixer = { .open = mixer_open };

void hl_audio_oss_register_devices(void) {
    static const hl_device_node_t nodes[] = {
        { "dsp",   &ops_dsp,   0020000 | 0666, 14, 3 },
        { "dsp0",  &ops_dsp,   0020000 | 0666, 14, 3 },
        { "audio", &ops_dsp,   0020000 | 0666, 14, 4 },
        { "mixer", &ops_mixer, 0020000 | 0666, 14, 0 },
    };
    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
        hl_device_register(&nodes[i]);
}

void hl_audio_oss_init(void) {
    hl_audio_init();
}

int hl_oss_fd_needs_recreate(int fd_type) {
    return fd_type == FD_OSS_DSP || fd_type == FD_OSS_MIXER;
}

int hl_oss_fork_export(hl_open_file_t *of, hl_fork_object_record_t *out) {
    if (!of || !out) return -1;
    if (of->kind == FD_OSS_DSP)
        return oss_fork_export(of, out);
    if (of->kind == FD_OSS_MIXER) {
        /* Mixer: export global levels only (stateless recreate). */
        memset(out, 0, sizeof(*out));
        out->kind = FD_OSS_MIXER;
        out->status_flags = atomic_load(&of->status_flags);
        out->object_id = of->object_id;
        out->payload_len = 0;
        return 0;
    }
    return -1;
}

hl_open_file_t *hl_oss_fork_import(const hl_fork_object_record_t *in) {
    if (!in) return NULL;
    if (in->kind == FD_OSS_DSP)
        return oss_fork_import(in, NULL);
    if (in->kind == FD_OSS_MIXER) {
        oss_mixer_state_t *st = calloc(1, sizeof(*st));
        if (!st) return NULL;
        *st = g_mixer;
        return hl_open_file_create(FD_OSS_MIXER, &oss_mixer_ops,
                                   in->status_flags, st);
    }
    return NULL;
}
