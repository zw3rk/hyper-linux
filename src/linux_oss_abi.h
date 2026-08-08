/* linux_oss_abi.h — Minimal Linux OSS UAPI constants and structures
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from Linux kernel UAPI headers (soundcard.h) for syscall
 * translation notes only. Linux is GPL-2.0; this is a clean-room
 * extraction of numeric ABI constants and structure layouts required
 * for userspace ioctl compatibility. AArch64 and x86_64 share this
 * generic layout for these ioctls.
 *
 * Reference: include/uapi/linux/soundcard.h (Linux kernel)
 * Do NOT pass these request values to Darwin ioctl(2).
 */
#ifndef LINUX_OSS_ABI_H
#define LINUX_OSS_ABI_H

#include <stdint.h>
#include <stddef.h>

/* ---- ioctl encoding (Linux asm-generic) ---- */
#define _LINUX_IOC_NRBITS   8
#define _LINUX_IOC_TYPEBITS 8
#define _LINUX_IOC_SIZEBITS 14
#define _LINUX_IOC_DIRBITS  2

#define _LINUX_IOC_NRSHIFT   0
#define _LINUX_IOC_TYPESHIFT (_LINUX_IOC_NRSHIFT + _LINUX_IOC_NRBITS)
#define _LINUX_IOC_SIZESHIFT (_LINUX_IOC_TYPESHIFT + _LINUX_IOC_TYPEBITS)
#define _LINUX_IOC_DIRSHIFT  (_LINUX_IOC_SIZESHIFT + _LINUX_IOC_SIZEBITS)

#define _LINUX_IOC_NONE  0U
#define _LINUX_IOC_WRITE 1U
#define _LINUX_IOC_READ  2U

#define _LINUX_IOC(dir, type, nr, size) \
    (((dir)  << _LINUX_IOC_DIRSHIFT) | \
     ((type) << _LINUX_IOC_TYPESHIFT) | \
     ((nr)   << _LINUX_IOC_NRSHIFT) | \
     ((size) << _LINUX_IOC_SIZESHIFT))

#define _LINUX_IO(type, nr)        _LINUX_IOC(_LINUX_IOC_NONE, (type), (nr), 0)
#define _LINUX_IOR(type, nr, t)    _LINUX_IOC(_LINUX_IOC_READ, (type), (nr), sizeof(t))
#define _LINUX_IOW(type, nr, t)    _LINUX_IOC(_LINUX_IOC_WRITE, (type), (nr), sizeof(t))
#define _LINUX_IOWR(type, nr, t)   _LINUX_IOC(_LINUX_IOC_READ|_LINUX_IOC_WRITE, (type), (nr), sizeof(t))

/* ---- PCM formats ---- */
#define AFMT_QUERY      0x00000000
#define AFMT_MU_LAW     0x00000001
#define AFMT_A_LAW      0x00000002
#define AFMT_IMA_ADPCM  0x00000004
#define AFMT_U8         0x00000008
#define AFMT_S16_LE     0x00000010
#define AFMT_S16_BE     0x00000020
#define AFMT_S8         0x00000040
#define AFMT_U16_LE     0x00000080
#define AFMT_U16_BE     0x00000100

/* ---- DSP ioctls (XMMS Tier-1) ---- */
#define SNDCTL_DSP_RESET           _LINUX_IO('P', 0)
#define SNDCTL_DSP_SYNC            _LINUX_IO('P', 1)
#define SNDCTL_DSP_SPEED           _LINUX_IOWR('P', 2, int)
#define SNDCTL_DSP_STEREO          _LINUX_IOWR('P', 3, int)
#define SNDCTL_DSP_GETBLKSIZE      _LINUX_IOWR('P', 4, int)
#define SNDCTL_DSP_SETFMT          _LINUX_IOWR('P', 5, int)
#define SNDCTL_DSP_CHANNELS        _LINUX_IOWR('P', 6, int)
#define SOUND_PCM_WRITE_CHANNELS   SNDCTL_DSP_CHANNELS
#define SOUND_PCM_WRITE_RATE       SNDCTL_DSP_SPEED
#define SOUND_PCM_WRITE_BITS       SNDCTL_DSP_SETFMT
#define SNDCTL_DSP_POST            _LINUX_IO('P', 8)
#define SNDCTL_DSP_SUBDIVIDE       _LINUX_IOWR('P', 9, int)
#define SNDCTL_DSP_SETFRAGMENT     _LINUX_IOWR('P', 10, int)
#define SNDCTL_DSP_GETFMTS         _LINUX_IOR('P', 11, int)
#define SNDCTL_DSP_GETOSPACE       _LINUX_IOR('P', 12, audio_buf_info)
#define SNDCTL_DSP_GETISPACE       _LINUX_IOR('P', 13, audio_buf_info)
#define SNDCTL_DSP_NONBLOCK        _LINUX_IO('P', 14)
#define SNDCTL_DSP_GETCAPS         _LINUX_IOR('P', 15, int)
#define SNDCTL_DSP_GETODELAY       _LINUX_IOR('P', 23, int)
#define SNDCTL_DSP_GETPLAYVOL      _LINUX_IOR('P', 24, int)
#define SNDCTL_DSP_SETPLAYVOL      _LINUX_IOWR('P', 24, int)

/* ---- audio_buf_info ---- */
typedef struct audio_buf_info {
    int fragments;  /* # of available fragments (partially used not counted) */
    int fragstotal; /* Total # of fragments allocated */
    int fragsize;   /* Size of a fragment in bytes */
    int bytes;      /* Available space in bytes (includes partial fragment) */
} audio_buf_info;

/* ---- Mixer ---- */
#define SOUND_MIXER_VOLUME  0
#define SOUND_MIXER_PCM     4
#define SOUND_MASK_VOLUME   (1 << SOUND_MIXER_VOLUME)
#define SOUND_MASK_PCM      (1 << SOUND_MIXER_PCM)

#define MIXER_READ(dev)  _LINUX_IOR('M', (dev), int)
#define MIXER_WRITE(dev) _LINUX_IOWR('M', (dev), int)

#define SOUND_MIXER_READ_VOLUME   MIXER_READ(SOUND_MIXER_VOLUME)
#define SOUND_MIXER_WRITE_VOLUME  MIXER_WRITE(SOUND_MIXER_VOLUME)
#define SOUND_MIXER_READ_PCM      MIXER_READ(SOUND_MIXER_PCM)
#define SOUND_MIXER_WRITE_PCM     MIXER_WRITE(SOUND_MIXER_PCM)
/* Device-info mixer "channels" — Linux soundcard.h:
 *   SOUND_MIXER_RECMASK   0xfd
 *   SOUND_MIXER_DEVMASK   0xfe
 *   SOUND_MIXER_STEREODEVS 0xfb
 * (Swapping DEVMASK/RECMASK makes XMMS see zero devices and skip volume.) */
#define SOUND_MIXER_RECMASK_NR    0xfd
#define SOUND_MIXER_DEVMASK_NR    0xfe
#define SOUND_MIXER_STEREODEVS_NR 0xfb
#define SOUND_MIXER_READ_DEVMASK  MIXER_READ(SOUND_MIXER_DEVMASK_NR)
#define SOUND_MIXER_READ_RECMASK  MIXER_READ(SOUND_MIXER_RECMASK_NR)
#define SOUND_MIXER_READ_STEREODEVS MIXER_READ(SOUND_MIXER_STEREODEVS_NR)

/*
 * Legacy OSS ioctl encoding (SIOC_OUT/IN/INOUT at bits 29–30).
 * XMMS 1.2 ships Output/OSS/soundcard.h with this pre-asm-generic layout:
 *   SIOC_OUT=0x20000000, SIOC_IN=0x40000000, size in bits 16–28.
 * Modern Linux (and our MIXER_* above) use _IOC with DIR in bits 30–31
 * (READ=2 → 0x80000000, WRITE|READ=3 → 0xc0000000). Both appear in the
 * wild; accept either for mixer so stock libOSS.so works.
 */
#define OSS_LEGACY_SIOC_OUT   0x20000000u
#define OSS_LEGACY_SIOC_IN    0x40000000u
#define OSS_LEGACY_SIOC_INOUT 0x60000000u
#define OSS_LEGACY_SIOR(type, nr, size) \
    (OSS_LEGACY_SIOC_OUT | (((unsigned)(size) & 0x1fffu) << 16) | \
     ((unsigned)(type) << 8) | (unsigned)(nr))
#define OSS_LEGACY_SIOWR(type, nr, size) \
    (OSS_LEGACY_SIOC_INOUT | (((unsigned)(size) & 0x1fffu) << 16) | \
     ((unsigned)(type) << 8) | (unsigned)(nr))

#define SOUND_MIXER_READ_VOLUME_LEGACY   OSS_LEGACY_SIOR('M', SOUND_MIXER_VOLUME, 4)
#define SOUND_MIXER_WRITE_VOLUME_LEGACY  OSS_LEGACY_SIOWR('M', SOUND_MIXER_VOLUME, 4)
#define SOUND_MIXER_READ_PCM_LEGACY      OSS_LEGACY_SIOR('M', SOUND_MIXER_PCM, 4)
#define SOUND_MIXER_WRITE_PCM_LEGACY     OSS_LEGACY_SIOWR('M', SOUND_MIXER_PCM, 4)
#define SOUND_MIXER_READ_DEVMASK_LEGACY  OSS_LEGACY_SIOR('M', SOUND_MIXER_DEVMASK_NR, 4)
#define SOUND_MIXER_READ_RECMASK_LEGACY  OSS_LEGACY_SIOR('M', SOUND_MIXER_RECMASK_NR, 4)
#define SOUND_MIXER_READ_STEREODEVS_LEGACY OSS_LEGACY_SIOR('M', SOUND_MIXER_STEREODEVS_NR, 4)

/* Compile-time ABI checks */
_Static_assert(sizeof(audio_buf_info) == 16, "audio_buf_info size");
_Static_assert(offsetof(audio_buf_info, fragments) == 0, "fragments offset");
_Static_assert(offsetof(audio_buf_info, bytes) == 12, "bytes offset");

/* SNDCTL_DSP_SETFMT value */
_Static_assert(AFMT_S16_LE == 0x10, "AFMT_S16_LE");
_Static_assert(AFMT_U8 == 0x08, "AFMT_U8");

#endif /* LINUX_OSS_ABI_H */
