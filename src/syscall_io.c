/* syscall_io.c — Core I/O syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read/write, ioctl, splice, sendfile, and copy_file_range operations.
 * All functions are called from syscall_dispatch() in syscall.c.
 *
 * Poll/select/epoll handlers are in syscall_poll.c.
 * Special FD types (eventfd, signalfd, timerfd) are in syscall_fd.c.
 */
#include "syscall_io.h"
#include "syscall_fd.h"
#include "syscall_inotify.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "syscall_proc.h"
#include "syscall_signal.h"
#include "guest.h"
#include "thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <pthread.h>
#include <termios.h>
#include <CommonCrypto/CommonDigest.h>

/* ---------- Linux terminal struct types ---------- */

/* Linux struct winsize (same layout as macOS) */
typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} linux_winsize_t;

/* Linux struct termios (aarch64): c_iflag..c_lflag are uint32_t,
 * c_line is uint8_t, c_cc has 19 entries, then speed fields.
 * macOS termios layout differs, so we translate field by field. */
typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
    uint32_t c_ispeed;  /* input speed (not in POSIX, but Linux has it) */
    uint32_t c_ospeed;  /* output speed */
} linux_termios_t;

/* ---------- basic read/write ---------- */

int64_t sys_write(guest_t *g, int fd, uint64_t buf_gva, uint64_t count) {
    if (fd >= 0 && fd < FD_TABLE_SIZE && fd_table[fd].type == FD_EVENTFD)
        return eventfd_write(fd, g, buf_gva, count);

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux: write(fd, NULL, 0) returns 0, not EFAULT */
    if (count == 0) return 0;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    /* Detect rosetta assertion message and dump thread context when verbose.
     * Rosetta writes "BasicBlock requested for unrecognized address" to
     * stderr (fd 2) right before crashing. Dump X18 (thread context ptr)
     * and the saved registers to help identify the failing x86 target. */
    if (hl_verbose && host_fd == STDERR_FILENO && count > 10 && count < 256) {
        if (memmem(buf, count, "BasicBlock", 10) != NULL) {
            fprintf(stderr, "hl: DEBUG assertion intercepted: %.*s\n",
                    (int)count, (char *)buf);
            /* Read X18 (thread context pointer) from the vCPU */
            hv_vcpu_t vcpu = current_thread->vcpu;
            uint64_t x18_val;
            hv_vcpu_get_reg(vcpu, HV_REG_X18, &x18_val);
            uint64_t x18_base = x18_val & 0x7FFFFFFFFFFFFFFFULL; /* clear bit 63 */
            fprintf(stderr, "hl: DEBUG X18=0x%llx (base=0x%llx)\n",
                    (unsigned long long)x18_val, (unsigned long long)x18_base);

            /* Dump saved registers from thread context [x18+0x10..0x90] */
            for (int i = 0; i < 16; i++) {
                uint64_t val = 0;
                void *p = guest_ptr(g, x18_base + 0x10 + i * 8);
                if (p) memcpy(&val, p, 8);
                fprintf(stderr, "hl: DEBUG ctx[X%d] = 0x%llx\n", i, (unsigned long long)val);
            }
            /* Dump key thread context fields beyond saved regs */
            struct { int offset; const char *name; } ctx_fields[] = {
                {0x138, "feat_afp"},
                {0x190, "thread_id"},
                {0x198, "hash_table_desc"},
                {0x1a0, "jit_region_lo"},
                {0x1a8, "target_x86_pc"},
                {0x1b0, "syscall_ret"},
                {0x1b8, "pending_flags"},
                {0x1c0, "misc_1c0"},
                {0x1c8, "signal_futex"},
                {0x1d0, "resume_pc"},
                {0x1d8, "misc_1d8"},
                {0x1e0, "translation_mode"},
            };
            for (int i = 0; i < (int)(sizeof(ctx_fields)/sizeof(ctx_fields[0])); i++) {
                uint64_t val = 0;
                void *p = guest_ptr(g, x18_base + ctx_fields[i].offset);
                if (p) memcpy(&val, p, 8);
                fprintf(stderr, "hl: DEBUG ctx[+0x%03x] %s = 0x%llx\n",
                        ctx_fields[i].offset, ctx_fields[i].name,
                        (unsigned long long)val);
            }
            /* Walk FP chain for return addresses */
            uint64_t fp, sp;
            hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &sp);
            hv_vcpu_get_reg(vcpu, HV_REG_FP, &fp);
            fprintf(stderr, "hl: DEBUG FP chain (SP=0x%llx, FP=0x%llx):\n",
                    (unsigned long long)sp, (unsigned long long)fp);
            for (int i = 0; i < 20 && fp != 0; i++) {
                uint64_t saved_fp = 0, saved_lr = 0;
                void *p = guest_ptr(g, fp);
                if (!p) break;
                memcpy(&saved_fp, p, 8);
                memcpy(&saved_lr, (uint8_t*)p + 8, 8);
                fprintf(stderr, "hl: DEBUG  [%d] FP=0x%llx LR=0x%llx\n",
                        i, (unsigned long long)saved_fp,
                        (unsigned long long)saved_lr);
                /* Also dump the callee-saved regs at FP-N */
                /* X19-X28 are saved below FP by ARM64 convention */
                for (int j = 0; j < 5; j++) {
                    uint64_t reg = 0;
                    void *rp = guest_ptr(g, fp - (j + 1) * 16);
                    if (!rp) break;
                    memcpy(&reg, rp, 8);
                    uint64_t reg2 = 0;
                    memcpy(&reg2, (uint8_t*)rp + 8, 8);
                    fprintf(stderr, "hl: DEBUG    [FP-0x%02x] 0x%llx, 0x%llx\n",
                            (j + 1) * 16, (unsigned long long)reg,
                            (unsigned long long)reg2);
                }
                fp = saved_fp;
            }

            /* Dump AOT metadata from guest memory at 0x418000 to verify
             * correct loading. The block table starts at 0x418140. */
            fprintf(stderr, "hl: DEBUG AOT metadata verification:\n");
            for (uint64_t gva = 0x418000; gva < 0x418200; gva += 16) {
                void *mp = guest_ptr(g, gva);
                if (!mp) { fprintf(stderr, "  [0x%llx] <unmapped>\n",
                           (unsigned long long)gva); break; }
                uint64_t v1 = 0, v2 = 0;
                memcpy(&v1, mp, 8);
                memcpy(&v2, (uint8_t*)mp + 8, 8);
                fprintf(stderr, "  [0x%llx] %016llx %016llx\n",
                        (unsigned long long)gva,
                        (unsigned long long)v1, (unsigned long long)v2);
            }
            /* Also dump the x86 .text content at 0x401000 (first 64 bytes)
             * and AOT code at 0x40a000 (first 64 bytes) */
            fprintf(stderr, "hl: DEBUG x86 .text at 0x401000:\n  ");
            void *tp = guest_ptr(g, 0x401000);
            if (tp) {
                for (int j = 0; j < 32; j++)
                    fprintf(stderr, "%02x ", ((uint8_t*)tp)[j]);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "hl: DEBUG AOT code at 0x40a000:\n  ");
            void *ap = guest_ptr(g, 0x40a000);
            if (ap) {
                for (int j = 0; j < 32; j++)
                    fprintf(stderr, "%02x ", ((uint8_t*)ap)[j]);
                fprintf(stderr, "\n");
            }
            /* Dump block table header at 0x418140 */
            fprintf(stderr, "hl: DEBUG block table at 0x418140:\n");
            for (uint64_t gva = 0x418140; gva < 0x418280; gva += 16) {
                void *bp = guest_ptr(g, gva);
                if (!bp) break;
                uint64_t v1 = 0, v2 = 0;
                memcpy(&v1, bp, 8);
                memcpy(&v2, (uint8_t*)bp + 8, 8);
                fprintf(stderr, "  [0x%llx] %016llx %016llx\n",
                        (unsigned long long)gva,
                        (unsigned long long)v1, (unsigned long long)v2);
            }

            /* Walk FP chain to find block_for_offset's frame.
             *
             * Call stack at assertion write time:
             *   write() syscall        ← current frame
             *   log_fatal (0x800000088c28)
             *   assert_fail_with_msg (0x8000000899e4)
             *   block_for_offset (0x8000000387d4)  ← WE WANT THIS
             *   translate_block (0x800000046cb0)
             *
             * block_for_offset's saved_lr = 0x800000047c08 (or 0x47abc/0x47c70)
             * (return address in the caller after bl 0x8000000387d4)
             *
             * block_for_offset's stack frame (0x50 bytes):
             *   FP+0x00: saved x29 (caller's FP)
             *   FP+0x08: saved x30 (return to caller)
             *   FP+0x10: saved x26, x25
             *   FP+0x20: saved x24, x23
             *   FP+0x30: saved x22, x21
             *   FP+0x40: saved x20  ← binary search result (0 = NULL)
             *   FP+0x48: saved x19  ← self (BuilderBase*)
             *
             * translate_block's frame (0x60 + 0x370 locals):
             *   FP+0x50: saved x20, x19
             *   FP+0x40: saved x22, x21  ← x22 = target x86 offset!
             *   FP+0x30: saved x24, x23
             *   FP+0x20: saved x26, x25
             *   FP+0x10: saved x28, x27
             */
            uint64_t walk_fp = 0;
            hv_vcpu_get_reg(vcpu, HV_REG_FP, &walk_fp);
            for (int frame_i = 0; frame_i < 20 && walk_fp != 0; frame_i++) {
                uint64_t sf = 0, sl = 0;
                void *wp = guest_ptr(g, walk_fp);
                if (!wp) break;
                memcpy(&sf, wp, 8);
                memcpy(&sl, (uint8_t*)wp + 8, 8);
                fprintf(stderr, "hl: DEBUG FPwalk[%d] fp=0x%llx "
                        "saved_fp=0x%llx saved_lr=0x%llx\n",
                        frame_i, (unsigned long long)walk_fp,
                        (unsigned long long)sf, (unsigned long long)sl);

                /* Strategy: find assert_fail_with_msg frame (LR in
                 * 0x800000038838..0x800000038864 = block_for_offset's
                 * assertion path), then read block_for_offset's active
                 * regs from assert_fail_with_msg's saved callee-regs.
                 *
                 * assert_fail_with_msg frame layout (0x40 bytes):
                 *   FP+0x00: x29,x30  FP+0x10: x28
                 *   FP+0x20: x22,x21  FP+0x30: x20,x19
                 * These are block_for_offset's ACTIVE values:
                 *   x19 = self (BuilderBase*)
                 *   x20 = 0 (binary search returned NULL)
                 *   x22 = whatever block_for_offset had in x22
                 *
                 * block_for_offset frame layout (0x50 bytes):
                 *   FP+0x00: x29,x30  FP+0x10: x26,x25
                 *   FP+0x20: x24,x23  FP+0x30: x22,x21
                 *   FP+0x40: x20,x19
                 * These are translate_block's values at call time:
                 *   x22 at FP+0x30 = w22 = TARGET x86 offset!
                 */
                if (sl >= 0x800000038838 && sl <= 0x800000038868) {
                    /* assert_fail_with_msg frame → read block_for_offset's
                     * active x19 (self) from FP+0x38. */
                    fprintf(stderr, "hl: DEBUG found assert_fail frame "
                            "(frame %d, FP=0x%llx, LR=0x%llx)\n",
                            frame_i, (unsigned long long)walk_fp,
                            (unsigned long long)sl);

                    uint64_t bfo_x19 = 0, bfo_x20 = 0;
                    void *p;
                    p = guest_ptr(g, walk_fp + 0x38);
                    if (p) memcpy(&bfo_x19, p, 8);
                    p = guest_ptr(g, walk_fp + 0x30);
                    if (p) memcpy(&bfo_x20, p, 8);

                    fprintf(stderr, "hl: DEBUG block_for_offset active regs "
                            "(from assert_fail frame):\n"
                            "  x19 (self)   = 0x%llx\n"
                            "  x20 (result) = 0x%llx\n",
                            (unsigned long long)bfo_x19,
                            (unsigned long long)bfo_x20);

                    /* Now get the x86 offset target. It was passed as w1
                     * to block_for_offset. In block_for_offset, w2=w1
                     * (copy for binary search). But both w1 and w2 are
                     * caller-saved and clobbered by the assertion setup.
                     *
                     * However, translate_block had w22 = the target offset.
                     * block_for_offset saves translate_block's x22 at
                     * block_for_offset's FP+0x30 (saved x22).
                     * block_for_offset's FP = saved_fp from this frame. */
                    uint64_t bfo_fp = sf; /* block_for_offset's FP */
                    if (bfo_fp) {
                        uint64_t saved_x22 = 0, saved_x23 = 0;
                        uint64_t saved_x19_entry = 0, saved_x20_entry = 0;
                        p = guest_ptr(g, bfo_fp + 0x30);
                        if (p) memcpy(&saved_x22, p, 8);
                        p = guest_ptr(g, bfo_fp + 0x38);
                        if (p) memcpy(&saved_x23, p, 8);
                        p = guest_ptr(g, bfo_fp + 0x48);
                        if (p) memcpy(&saved_x19_entry, p, 8);
                        p = guest_ptr(g, bfo_fp + 0x40);
                        if (p) memcpy(&saved_x20_entry, p, 8);

                        /* Also read w1 from block_for_offset:
                         * At entry, w2=w1 then x9=self->current_offset.
                         * current_offset is at self+0x10. */
                        uint64_t cur_off = 0;
                        if (bfo_x19) {
                            p = guest_ptr(g, bfo_x19 + 0x10);
                            if (p) memcpy(&cur_off, p, 8);
                        }

                        fprintf(stderr, "hl: DEBUG block_for_offset frame "
                                "(FP=0x%llx):\n"
                                "  saved x22 (translate_block's x22) = 0x%llx  "
                                "← TARGET x86 offset\n"
                                "  saved x21 (translate_block's x21) = 0x%llx\n"
                                "  saved x19 (translate_block's x19) = 0x%llx\n"
                                "  saved x20 (translate_block's x20) = 0x%llx\n"
                                "  self->current_offset (+0x10)      = 0x%llx\n",
                                (unsigned long long)bfo_fp,
                                (unsigned long long)saved_x22,
                                (unsigned long long)saved_x23,
                                (unsigned long long)saved_x19_entry,
                                (unsigned long long)saved_x20_entry,
                                (unsigned long long)cur_off);
                    }

                    /* Dump TranslationBuilder state from self (x19) */
                    if (bfo_x19) {
                        void *bp;
                        uint64_t cur_off = 0, blk_begin = 0, blk_end = 0;
                        uint64_t code_buf = 0;
                        bp = guest_ptr(g, bfo_x19 + 0x10);
                        if (bp) memcpy(&cur_off, bp, 8);
                        bp = guest_ptr(g, bfo_x19 + 0x178);
                        if (bp) memcpy(&blk_begin, bp, 8);
                        bp = guest_ptr(g, bfo_x19 + 0x180);
                        if (bp) memcpy(&blk_end, bp, 8);
                        bp = guest_ptr(g, bfo_x19 + 0x1c0);
                        if (bp) memcpy(&code_buf, bp, 8);

                        uint64_t nblocks = (blk_end > blk_begin) ?
                                           (blk_end - blk_begin) / 8 : 0;
                        fprintf(stderr, "hl: DEBUG TranslationBuilder "
                                "(self=0x%llx):\n"
                                "  current_offset  = 0x%llx\n"
                                "  blocks          = [0x%llx..0x%llx] "
                                "(%llu blocks)\n"
                                "  code_buffer     = 0x%llx\n",
                                (unsigned long long)bfo_x19,
                                (unsigned long long)cur_off,
                                (unsigned long long)blk_begin,
                                (unsigned long long)blk_end,
                                (unsigned long long)nblocks,
                                (unsigned long long)code_buf);

                        /* Dump all BasicBlock x86_offsets */
                        for (uint64_t i = 0; i < nblocks && i < 64; i++) {
                            uint64_t bb_ptr = 0;
                            bp = guest_ptr(g, blk_begin + i * 8);
                            if (!bp) break;
                            memcpy(&bb_ptr, bp, 8);
                            uint32_t x86_off = 0;
                            bp = guest_ptr(g, bb_ptr + 0x40);
                            if (bp) memcpy(&x86_off, bp, 4);
                            fprintf(stderr, "  block[%llu] ptr=0x%llx "
                                    "x86_off=0x%x\n",
                                    (unsigned long long)i,
                                    (unsigned long long)bb_ptr,
                                    (unsigned)x86_off);
                        }

                        /* Dump more TranslationBuilder fields to
                         * find the x86 source base address */
                        fprintf(stderr, "hl: DEBUG TranslationBuilder "
                                "fields:\n");
                        for (int off = 0; off < 0x200; off += 8) {
                            uint64_t val = 0;
                            bp = guest_ptr(g, bfo_x19 + off);
                            if (bp) memcpy(&val, bp, 8);
                            if (val != 0)
                                fprintf(stderr, "  +0x%03x: 0x%llx\n",
                                        off, (unsigned long long)val);
                        }

                        /* Hex dump x86 code around the desync.
                         * code_buffer may or may not be the x86 src.
                         * Dump 64 bytes starting 16 before the
                         * target offset from BOTH code_buffer and
                         * what might be the x86 base. */
                        uint64_t tgt_off = 0;
                        if (bfo_fp) {
                            p = guest_ptr(g, bfo_fp + 0x30);
                            if (p) memcpy(&tgt_off, p, 8);
                            tgt_off &= 0xFFFFFFFF;
                        }
                        uint64_t dump_start = (tgt_off > 0x20) ?
                                              tgt_off - 0x20 : 0;
                        fprintf(stderr, "hl: DEBUG x86 code at "
                                "code_buffer+0x%llx..+0x%llx "
                                "(GVA 0x%llx):\n",
                                (unsigned long long)dump_start,
                                (unsigned long long)(dump_start + 0x60),
                                (unsigned long long)(code_buf + dump_start));
                        for (uint64_t off = dump_start;
                             off < dump_start + 0x60; off += 16) {
                            uint8_t hex[16] = {0};
                            p = guest_ptr(g, code_buf + off);
                            if (!p) break;
                            memcpy(hex, p, 16);
                            fprintf(stderr, "  +%03llx: ",
                                    (unsigned long long)off);
                            for (int j = 0; j < 16; j++)
                                fprintf(stderr, "%02x ", hex[j]);
                            fprintf(stderr, "\n");
                        }
                    }
                    break;
                }
                walk_fp = sf;
            }
        }
    }

    ssize_t ret = write(host_fd, buf, count);
    if (ret < 0) {
        if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        return linux_errno();
    }
    return ret;
}

int64_t sys_read(guest_t *g, int fd, uint64_t buf_gva, uint64_t count) {
    if (fd >= 0 && fd < FD_TABLE_SIZE) {
        if (fd_table[fd].type == FD_EVENTFD)
            return eventfd_read(fd, g, buf_gva, count);
        if (fd_table[fd].type == FD_SIGNALFD)
            return signalfd_read(fd, g, buf_gva, count);
        if (fd_table[fd].type == FD_TIMERFD)
            return timerfd_read(fd, g, buf_gva, count);
        if (fd_table[fd].type == FD_INOTIFY)
            return inotify_read(fd, g, buf_gva, count);
    }

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux: read(fd, NULL, 0) returns 0, not EFAULT */
    if (count == 0) return 0;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = read(host_fd, buf, count);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_pread64(guest_t *g, int fd, uint64_t buf_gva,
                    uint64_t count, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux: pread(fd, NULL, 0, off) returns 0, not EFAULT */
    if (count == 0) return 0;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = pread(host_fd, buf, count, offset);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_pwrite64(guest_t *g, int fd, uint64_t buf_gva,
                     uint64_t count, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux: pwrite(fd, NULL, 0, off) returns 0, not EFAULT */
    if (count == 0) return 0;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = pwrite(host_fd, buf, count, offset);
    if (ret < 0) {
        if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        return linux_errno();
    }
    return ret;
}

/* Helper: build host iovec array from guest iovec array.
 * Returns 0 on success, -LINUX_EFAULT on bad guest pointer. */
static int64_t build_host_iov(guest_t *g, uint64_t iov_gva, int iovcnt,
                               struct iovec *host_iov) {
    linux_iovec_t *guest_iov = guest_ptr(g, iov_gva);
    if (!guest_iov) return -LINUX_EFAULT;
    for (int i = 0; i < iovcnt; i++) {
        void *base = guest_ptr(g, guest_iov[i].iov_base);
        if (!base && guest_iov[i].iov_len > 0) return -LINUX_EFAULT;
        uint64_t iov_end = guest_iov[i].iov_base + guest_iov[i].iov_len;
        if (iov_end < guest_iov[i].iov_base) return -LINUX_EFAULT;
        if (guest_iov[i].iov_len > 0 && !guest_ptr(g, iov_end - 1))
            return -LINUX_EFAULT;
        host_iov[i].iov_base = base;
        host_iov[i].iov_len = guest_iov[i].iov_len;
    }
    return 0;
}

int64_t sys_readv(guest_t *g, int fd, uint64_t iov_gva, int iovcnt) {
    /* Special FD types need their custom read handlers — glibc may use
     * readv() instead of read() for the same logical operation. Delegate
     * to the first iov entry's buffer.  Use the first iov's length (not
     * the sum of all iovs) because the data goes into giov[0].iov_base
     * which is only giov[0].iov_len bytes long. */
    if (fd >= 0 && fd < FD_TABLE_SIZE) {
        int type = fd_table[fd].type;
        if (type == FD_EVENTFD || type == FD_SIGNALFD ||
            type == FD_TIMERFD || type == FD_INOTIFY) {
            if (iovcnt <= 0) return -LINUX_EINVAL;
            linux_iovec_t *giov = guest_ptr(g, iov_gva);
            if (!giov) return -LINUX_EFAULT;
            return sys_read(g, fd, giov[0].iov_base, giov[0].iov_len);
        }
    }

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));
    int64_t err = build_host_iov(g, iov_gva, iovcnt, host_iov);
    if (err < 0) return err;

    ssize_t ret = readv(host_fd, host_iov, iovcnt);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_writev(guest_t *g, int fd, uint64_t iov_gva, int iovcnt) {
    /* Special FD types: glibc may use writev() for eventfd wakeup writes.
     * Delegate using the first iov entry.  Use giov[0].iov_len (not the
     * sum of all iovs) — the data is at giov[0].iov_base which is only
     * giov[0].iov_len bytes.  eventfd expects exactly 8 bytes. */
    if (fd >= 0 && fd < FD_TABLE_SIZE && fd_table[fd].type == FD_EVENTFD) {
        if (iovcnt <= 0) return -LINUX_EINVAL;
        linux_iovec_t *giov = guest_ptr(g, iov_gva);
        if (!giov) return -LINUX_EFAULT;
        return eventfd_write(fd, g, giov[0].iov_base, giov[0].iov_len);
    }

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));
    int64_t err = build_host_iov(g, iov_gva, iovcnt, host_iov);
    if (err < 0) return err;

    ssize_t ret = writev(host_fd, host_iov, iovcnt);
    if (ret < 0) {
        if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        return linux_errno();
    }
    return ret;
}

int64_t sys_preadv(guest_t *g, int fd, uint64_t iov_gva,
                   int iovcnt, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));
    int64_t err = build_host_iov(g, iov_gva, iovcnt, host_iov);
    if (err < 0) return err;

    ssize_t ret = preadv(host_fd, host_iov, iovcnt, offset);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_pwritev(guest_t *g, int fd, uint64_t iov_gva,
                    int iovcnt, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));
    int64_t err = build_host_iov(g, iov_gva, iovcnt, host_iov);
    if (err < 0) return err;

    ssize_t ret = pwritev(host_fd, host_iov, iovcnt, offset);
    if (ret < 0) {
        if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        return linux_errno();
    }
    return ret;
}

int64_t sys_preadv2(guest_t *g, int fd, uint64_t iov_gva,
                    int iovcnt, int64_t offset, int flags) {
    /* preadv2 extends preadv with a flags parameter (RWF_HIPRI,
     * RWF_DSYNC, etc.). macOS has no preadv2 equivalent, so we
     * ignore the flags and delegate to preadv. If offset is -1,
     * use the current file position (like readv). */
    (void)flags;
    if (offset == -1)
        return sys_readv(g, fd, iov_gva, iovcnt);
    return sys_preadv(g, fd, iov_gva, iovcnt, offset);
}

int64_t sys_pwritev2(guest_t *g, int fd, uint64_t iov_gva,
                     int iovcnt, int64_t offset, int flags) {
    (void)flags;
    if (offset == -1)
        return sys_writev(g, fd, iov_gva, iovcnt);
    return sys_pwritev(g, fd, iov_gva, iovcnt, offset);
}

/* ---------- rosettad socket tracking ---------- */

/* Rosetta connects to rosettad via AF_UNIX SOCK_SEQPACKET for AOT
 * (ahead-of-time) translation. macOS doesn't support SOCK_SEQPACKET
 * for AF_UNIX, so we intercept the socket creation in sys_socket()
 * (syscall_net.c) with a socketpair(SOCK_STREAM). One end goes to
 * rosetta (the client), the other to our rosettad_handler_thread.
 * connect() is intercepted to return success immediately. */
static char rosettad_binary_path[LINUX_PATH_MAX] = {0}; /* x86_64 binary for on-demand AOT */
static int rosettad_client_fd = -1;  /* Rosetta's end of socketpair (host fd) */

/* Receive a file descriptor via SCM_RIGHTS ancillary data.
 * Also reads the normal data payload into buf (up to buflen).
 * Returns bytes of normal data received, or -1 on error.
 * On success, *recv_fd is set to the received fd (-1 if none). */
static ssize_t recv_fd(int sock, void *buf, size_t buflen, int *recv_fd_out) {
    struct iovec iov = { .iov_base = buf, .iov_len = buflen };
    uint8_t cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cmsg_buf, .msg_controllen = sizeof(cmsg_buf)
    };

    *recv_fd_out = -1;
    ssize_t n = recvmsg(sock, &msg, 0);
    if (n <= 0) return n;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS &&
        cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
        memcpy(recv_fd_out, CMSG_DATA(cmsg), sizeof(int));
    }
    return n;
}

/* Send a file descriptor via SCM_RIGHTS ancillary data.
 * Also sends normal data from buf (buflen bytes).
 * Returns bytes sent or -1 on error. */
static ssize_t send_fd(int sock, const void *buf, size_t buflen, int send_fd_val) {
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = buflen };
    uint8_t cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cmsg_buf, .msg_controllen = sizeof(cmsg_buf)
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &send_fd_val, sizeof(int));

    return sendmsg(sock, &msg, 0);
}

/* Compute SHA256 digest of a file by fd (seeks back to start after).
 * Returns 0 on success, -1 on error. */
static int compute_fd_sha256(int fd, uint8_t digest[CC_SHA256_DIGEST_LENGTH]) {
    off_t saved = lseek(fd, 0, SEEK_CUR);
    lseek(fd, 0, SEEK_SET);

    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);

    uint8_t buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n);

    lseek(fd, saved, SEEK_SET);
    if (n < 0) return -1;

    CC_SHA256_Final(digest, &ctx);
    return 0;
}

/* ---------- Persistent AOT cache ---------- */

/* Cache directory: ~/.cache/hl-rosettad/
 * Files: <sha256_hex>.aot — keyed by SHA256 of the original x86_64 binary.
 * This matches real rosettad behavior: the digest rosetta stores in .flu
 * files (and sends via 'd') is the SHA256 of the binary, not the AOT. */
static char aot_cache_dir[PATH_MAX] = {0};

/* Initialize the persistent AOT cache directory.
 * Creates ~/.cache/hl-rosettad/ if it doesn't exist. */
static void aot_cache_init(void) {
    if (aot_cache_dir[0]) return;  /* already initialized */

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    snprintf(aot_cache_dir, sizeof(aot_cache_dir),
             "%s/.cache/hl-rosettad", home);
    mkdir(aot_cache_dir, 0700);  /* ignore EEXIST */
}

/* Format a SHA256 digest as hex into buf (must be >= 65 bytes). */
static void digest_to_hex(const uint8_t digest[32], char *buf) {
    for (int i = 0; i < 32; i++)
        snprintf(buf + i * 2, 3, "%02x", digest[i]);
}

/* Look up a cached AOT file by binary SHA256 digest.
 * Returns an open fd on hit, -1 on miss. */
static int aot_cache_lookup(const uint8_t digest[32]) {
    aot_cache_init();
    char hex[65];
    digest_to_hex(digest, hex);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.aot", aot_cache_dir, hex);
    return open(path, O_RDONLY);
}

/* Store an AOT file in the persistent cache, keyed by binary SHA256.
 * Moves (hard-links + unlinks) the temp file into the cache dir. */
static void aot_cache_store(const uint8_t digest[32], const char *aot_path) {
    aot_cache_init();
    char hex[65];
    digest_to_hex(digest, hex);

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/%s.aot", aot_cache_dir, hex);

    /* Try link+unlink for atomicity; fall back to rename */
    if (link(aot_path, dest) == 0) {
        unlink(aot_path);
    } else {
        rename(aot_path, dest);  /* cross-device fallback */
    }
}

/* Run 'hl rosettad translate <input> <output>' via posix_spawn().
 * Returns the child exit status, or -1 on spawn failure. */
static int run_rosettad_translate(const char *bin_path, const char *aot_path) {
    const char *hl_bin = proc_get_hl_path();
    if (!hl_bin) hl_bin = "hl";

    static const char rosettad_path[] =
        "/Library/Apple/usr/libexec/oah/RosettaLinux/rosettad";

    char *argv[] = {
        (char *)hl_bin, (char *)rosettad_path,
        "translate", (char *)bin_path, (char *)aot_path, NULL
    };

    /* Let child inherit stderr for diagnostic output */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    extern char **environ;
    pid_t pid;
    int err = posix_spawn(&pid, hl_bin, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    if (err != 0) {
        fprintf(stderr, "hl: rosettad: posix_spawn failed: %s\n",
                strerror(err));
        return -1;
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Handle the rosettad protocol on our end of the socketpair.
 *
 * Rosetta connects to rosettad via AF_UNIX SOCK_SEQPACKET for AOT
 * (ahead-of-time) translation of x86_64 code. Since macOS doesn't
 * support SOCK_SEQPACKET for AF_UNIX, we intercept via socketpair
 * (SOCK_STREAM) and frame messages by individual write() calls.
 *
 * Protocol:
 *   '?' → respond 0x01 (ready)
 *   't' → receive binary fd via SCM_RIGHTS, translate, send back AOT fd
 *   'd' → receive 32-byte digest, check cache, respond
 *   'q' → quit
 */
static void *rosettad_handler_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    if (hl_verbose)
        fprintf(stderr, "hl: rosettad: handler thread started (fd=%d)\n", fd);

    for (;;) {
        uint8_t cmd;
        ssize_t n = read(fd, &cmd, 1);
        if (n <= 0) {
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: read returned %zd (%s), "
                        "exiting\n", n, n < 0 ? strerror(errno) : "EOF");
            break;
        }

        switch (cmd) {
        case '?': {
            /* Handshake: respond 0x01 to enable AOT translation.
             * 0x01 = ready (enables rosettad AOT path)
             * 0x00 = not ready (JIT-only, no AOT) */
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: handshake '?'\n");
            uint8_t resp = 0x01;
            if (write(fd, &resp, 1) != 1) {
                fprintf(stderr, "hl: rosettad: handshake write failed\n");
                goto done;
            }
            break;
        }

        case 't': {
            /* Translate request: rosetta sends the binary fd via sendmsg
             * (SCM_RIGHTS) with a data payload. We compute the binary's
             * SHA256, check the persistent cache, and translate only on
             * cache miss. This matches real rosettad behavior where the
             * digest is always the SHA256 of the original binary. */
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: translate request 't'\n");
            uint8_t params[256];
            int bin_fd = -1;

            ssize_t rn = recv_fd(fd, params, sizeof(params), &bin_fd);
            if (rn <= 0 || bin_fd < 0) {
                fprintf(stderr, "hl: rosettad: recv_fd failed: n=%zd fd=%d (%s)\n",
                        rn, bin_fd, rn < 0 ? strerror(errno) : "no fd");
                if (bin_fd >= 0) close(bin_fd);
                uint8_t resp = 0x00;
                if (write(fd, &resp, 1) != 1) goto done;
                break;
            }
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: recv_fd got fd=%d, %zd bytes data\n",
                        bin_fd, rn);

            /* Skip AOT for large binaries (>100MB) early — avoid computing
             * SHA256 of 200MB+ files. Rosetta's AOT code path allocates
             * high-VA regions that exceed hl's current page table capacity
             * for very large binaries (e.g., pandoc at 217MB). */
            struct stat bin_st;
            if (fstat(bin_fd, &bin_st) == 0 && bin_st.st_size > 100 * 1024 * 1024) {
                if (hl_verbose)
                    fprintf(stderr, "hl: rosettad: skipping translate for "
                            "large binary (%lld bytes)\n",
                            (long long)bin_st.st_size);
                close(bin_fd);
                uint8_t resp = 0x00;
                if (write(fd, &resp, 1) != 1) goto done;
                break;
            }

            /* Compute SHA256 of the original binary (not the AOT output).
             * This is the digest rosetta stores in .flu files and sends
             * via 'd' for subsequent cache lookups. */
            uint8_t bin_digest[32];
            if (compute_fd_sha256(bin_fd, bin_digest) < 0) {
                fprintf(stderr, "hl: rosettad: SHA256 of binary failed\n");
                close(bin_fd);
                uint8_t resp = 0x00;
                if (write(fd, &resp, 1) != 1) goto done;
                break;
            }

            /* Check persistent cache — skip translation if already cached */
            int cached_fd = aot_cache_lookup(bin_digest);
            if (cached_fd >= 0) {
                if (hl_verbose) {
                    char hex[65];
                    digest_to_hex(bin_digest, hex);
                    fprintf(stderr, "hl: rosettad: cache HIT for %s\n", hex);
                }
                close(bin_fd);

                /* Send cached AOT: success + digest + fd */
                uint8_t resp = 0x01;
                if (write(fd, &resp, 1) != 1) { close(cached_fd); goto done; }
                if (write(fd, bin_digest, 32) != 32) { close(cached_fd); goto done; }
                uint8_t dummy = 0;
                ssize_t sent = send_fd(fd, &dummy, 1, cached_fd);
                if (sent < 0) {
                    fprintf(stderr, "hl: rosettad: send_fd (cached) failed: %s\n",
                            strerror(errno));
                    close(cached_fd);
                    goto done;
                }
                if (hl_verbose)
                    fprintf(stderr, "hl: rosettad: sent cached AOT fd=%d\n",
                            cached_fd);
                close(cached_fd);
                break;
            }

            /* Get the binary's path via F_GETPATH for translation */
            char bin_path[1024];
            if (fcntl(bin_fd, F_GETPATH, bin_path) < 0) {
                fprintf(stderr, "hl: rosettad: F_GETPATH failed: %s\n",
                        strerror(errno));
                uint8_t resp = 0x00;
                if (write(fd, &resp, 1) != 1) { close(bin_fd); goto done; }
                close(bin_fd);
                break;
            }
            close(bin_fd);
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: translating %s\n", bin_path);

            /* Create temp file for AOT output */
            char aot_path[] = "/tmp/hl-aot-XXXXXX";
            int aot_fd = mkstemp(aot_path);
            if (aot_fd < 0) {
                fprintf(stderr, "hl: rosettad: mkstemp failed: %s\n",
                        strerror(errno));
                uint8_t resp = 0x00;
                if (write(fd, &resp, 1) != 1) goto done;
                break;
            }
            close(aot_fd);

            int ret = run_rosettad_translate(bin_path, aot_path);
            if (ret != 0) {
                fprintf(stderr, "hl: rosettad: translate failed (exit=%d) "
                        "for %s\n", ret, bin_path);
                uint8_t resp = 0x00;
                if (write(fd, &resp, 1) != 1) { unlink(aot_path); goto done; }
                unlink(aot_path);
                break;
            }

            /* Store AOT in persistent cache (moves temp file into cache dir) */
            aot_cache_store(bin_digest, aot_path);

            /* Open the cached AOT file for sending */
            aot_fd = aot_cache_lookup(bin_digest);
            if (aot_fd < 0) {
                /* Fallback: try the original temp path (store may have failed) */
                aot_fd = open(aot_path, O_RDONLY);
            }
            if (aot_fd < 0) {
                fprintf(stderr, "hl: rosettad: open AOT failed after translate\n");
                uint8_t resp = 0x00;
                if (write(fd, &resp, 1) != 1) goto done;
                break;
            }
            if (hl_verbose) {
                struct stat st;
                if (fstat(aot_fd, &st) == 0)
                    fprintf(stderr, "hl: rosettad: AOT ready (%lld bytes) for %s\n",
                            (long long)st.st_size, bin_path);
            }

            /* Rosetta expects THREE separate messages for the translate
             * response (matching SOCK_SEQPACKET semantics where each
             * send/write creates a distinct message):
             *   1. Success byte (0x01)
             *   2. SHA256 digest of original binary (32 bytes)
             *   3. AOT fd via SCM_RIGHTS + 1-byte dummy
             *
             * IMPORTANT: Do NOT combine into one sendmsg — rosetta reads
             * these as three separate recvmsg/read calls. */
            uint8_t resp = 0x01;
            if (write(fd, &resp, 1) != 1) { close(aot_fd); goto done; }
            if (write(fd, bin_digest, 32) != 32) { close(aot_fd); goto done; }

            /* Send AOT fd via SCM_RIGHTS with 1-byte dummy payload. */
            uint8_t dummy = 0;
            ssize_t sent = send_fd(fd, &dummy, 1, aot_fd);
            if (sent < 0) {
                fprintf(stderr, "hl: rosettad: send_fd failed: %s\n",
                        strerror(errno));
                close(aot_fd);
                goto done;
            }
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: sent AOT fd=%d (%zd bytes meta)\n",
                        aot_fd, sent);
            close(aot_fd);
            break;
        }

        case 'd': {
            /* Digest lookup: receive 32-byte SHA256 of the original binary,
             * look up the persistent AOT cache.
             *
             * This matches real rosettad behavior: rosetta caches the binary
             * SHA256 in .flu files (~/.cache/rosetta/) and sends it via 'd'
             * on subsequent invocations. On HIT, we send the cached AOT fd
             * directly — this avoids re-translation and uses the 'd' HIT
             * code path in rosetta (which handles large binaries better
             * than the 't' response path). */
            uint8_t digest[32];
            if (read(fd, digest, 32) != 32) {
                fprintf(stderr, "hl: rosettad: digest read short\n");
                goto done;
            }

            int cached_fd = aot_cache_lookup(digest);
            if (cached_fd >= 0) {
                if (hl_verbose) {
                    char hex[65];
                    digest_to_hex(digest, hex);
                    fprintf(stderr, "hl: rosettad: digest lookup → HIT (%s)\n",
                            hex);
                }

                /* HIT response: success byte + AOT fd via SCM_RIGHTS */
                uint8_t resp = 0x01;
                if (write(fd, &resp, 1) != 1) { close(cached_fd); goto done; }
                uint8_t dummy = 0;
                ssize_t sent = send_fd(fd, &dummy, 1, cached_fd);
                if (sent < 0) {
                    fprintf(stderr, "hl: rosettad: send_fd (digest) failed: %s\n",
                            strerror(errno));
                    close(cached_fd);
                    goto done;
                }
                if (hl_verbose)
                    fprintf(stderr, "hl: rosettad: sent cached AOT fd=%d\n",
                            cached_fd);
                close(cached_fd);
            } else {
                if (hl_verbose) {
                    char hex[65];
                    digest_to_hex(digest, hex);
                    fprintf(stderr, "hl: rosettad: digest lookup → MISS (%s)\n",
                            hex);
                }
                uint8_t resp = 0x00;
                if (write(fd, &resp, 1) != 1) goto done;
            }
            break;
        }

        case 'q':
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: quit 'q'\n");
            goto done;

        default:
            fprintf(stderr, "hl: rosettad: unknown cmd 0x%02x\n", cmd);
            goto done;
        }
    }

done:
    if (hl_verbose)
        fprintf(stderr, "hl: rosettad: handler thread exiting\n");
    close(fd);
    return NULL;
}

void rosettad_set_binary_path(const char *path) {
    if (path) {
        strncpy(rosettad_binary_path, path, sizeof(rosettad_binary_path) - 1);
        rosettad_binary_path[sizeof(rosettad_binary_path) - 1] = '\0';
    }
}

void rosettad_start_handler(int handler_fd, int client_fd) {
    rosettad_client_fd = client_fd;
    if (hl_verbose)
        fprintf(stderr, "hl: rosettad: starting handler thread "
                "(handler_fd=%d, client_fd=%d)\n", handler_fd, client_fd);
    pthread_t thr;
    pthread_create(&thr, NULL, rosettad_handler_thread,
                   (void *)(intptr_t)handler_fd);
    pthread_detach(thr);
}

int rosettad_is_socket(int host_fd) {
    return rosettad_client_fd >= 0 && host_fd == rosettad_client_fd;
}

/* ---------- terminal I/O ---------- */

/* Rosetta Virtualization.framework ioctls — type 'a' (0x61).
 * In VZ Linux VMs, rosetta communicates with the macOS hypervisor via
 * custom ioctls on the virtiofs-mounted rosetta binary (opened via
 * /proc/self/exe). We emulate these to make rosetta work in HVF.
 *
 * 0x80456125: _IOR('a', 0x25, 69)  — environment signature check
 * 0x80806123: _IOR('a', 0x23, 128) — capabilities/config query
 * 0x6124:     _IO('a', 0x24)       — JIT activation / hypervisor handshake */
#define ROSETTA_VZ_CHECK      0x80456125
#define ROSETTA_VZ_CAPS       0x80806123
#define ROSETTA_VZ_ACTIVATE   0x6124

int64_t sys_ioctl(guest_t *g, int fd, uint64_t request, uint64_t arg) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    switch (request) {
    case ROSETTA_VZ_CHECK: {
        /* Rosetta environment check. Returns a 69-byte signature that
         * rosetta memcmp's against an embedded constant to verify it's
         * running in a supported Apple Virtualization.framework environment.
         * This MUST succeed — without it, rosetta prints "Rosetta is only
         * intended to run on Apple Silicon with a macOS host using
         * Virtualization.framework with Rosetta mode enabled" and aborts. */
        if (g->verbose)
            fprintf(stderr, "hl: rosetta: VZ_CHECK ioctl\n");
        static const char rosetta_sig[69] =
            "Our hard work\nby these words guarded\n"
            "please don't steal\n\xc2\xa9 Apple Inc";
        if (guest_write(g, arg, rosetta_sig, sizeof(rosetta_sig)) < 0)
            return -LINUX_EFAULT;
        return 1;  /* Real VZ driver returns 1 on success */
    }

    case ROSETTA_VZ_CAPS: {
        /* VZ_CAPS buffer layout (128 bytes).
         *
         * Verified via strace + ioctl dump in a real Lima VZ VM:
         *   Real Apple VZ returns: caps[0]=1,
         *   caps[1..27]="/run/rosettad/rosetta.sock\0",
         *   caps[28..127]=all zeros (including caps[64] and caps[108]).
         *
         *   caps[0]:       VZ enable flag (1 = VZ mode active).
         *                  Written to BSS[0xa04]; enables the rosettad AOT path.
         *
         *   caps[1..64]:   sun_path[0..63] — socket path for rosettad.
         *                  Must be non-empty (caps[1] != 0) for rosettad init
         *                  to proceed. The actual path doesn't matter because
         *                  connect() is intercepted in sys_connect() via
         *                  rosettad_is_socket() — the socketpair is pre-connected.
         *
         *   caps[66..107]: Null-terminated path to the x86_64 binary.
         *                  Rosetta opens this (caps+0x42) and sends the fd to
         *                  rosettad via SCM_RIGHTS for AOT translation.
         *
         *   caps[108]:     Written to BSS[0xa05]. Real VZ has this as 0.
         *                  Both 't' (translate) and 'd' (digest) protocol
         *                  commands work with caps[108]=0 (verified by strace).
         *
         *   caps[109..127]: Other flags (purpose unknown, leave as zero). */
        uint8_t caps[128] = {0};

        /* caps[0]: VZ enable flag — activates the rosettad AOT pipeline. */
        caps[0] = 1;

        /* caps[1..]: Socket path — must be non-empty for rosettad init.
         * We use a short placeholder; connect() is intercepted fd-based. */
        static const char fake_sock_path[] = "/run/rosettad/rosetta.sock";
        memcpy(&caps[1], fake_sock_path, sizeof(fake_sock_path));

        /* caps[66..]: Null-terminated path to x86_64 binary for rosettad.
         * Rosetta opens this file and sends the fd to rosettad via SCM_RIGHTS. */
        size_t binpath_len = strlen(rosettad_binary_path);
        if (binpath_len > 0 && binpath_len <= 128 - 66 - 1) {
            memcpy(&caps[66], rosettad_binary_path, binpath_len + 1);
        } else if (binpath_len > 0) {
            /* Path too long to fit: truncate, ensure null termination at [127] */
            memcpy(&caps[66], rosettad_binary_path, 128 - 66 - 1);
            caps[127] = 0;
        }

        /* caps[108]: Match real VZ behavior (0). Both 't' and 'd' protocol
         * commands work with this value, verified by strace in Lima VM. */
        caps[108] = 0;

        if (g->verbose)
            fprintf(stderr, "hl: rosetta: VZ_CAPS ioctl → caps[0]=%d caps[64]=0x%02x "
                    "caps[108]=0x%02x binary=%s\n",
                    caps[0], caps[64], caps[108], rosettad_binary_path);
        if (guest_write(g, arg, caps, sizeof(caps)) < 0)
            return -LINUX_EFAULT;
        return 1;  /* Real VZ driver returns 1 on success */
    }

    case ROSETTA_VZ_ACTIVATE:
        /* Rosetta JIT activation / hypervisor handshake. */
        if (g->verbose)
            fprintf(stderr, "hl: rosetta: VZ_ACTIVATE ioctl\n");
        return 1;  /* Real VZ driver returns 1 on success */

    case LINUX_TIOCGWINSZ: {
        /* Get terminal window size */
        struct winsize ws;
        if (ioctl(host_fd, TIOCGWINSZ, &ws) < 0)
            return -LINUX_ENOTTY;
        linux_winsize_t lws = {
            .ws_row = ws.ws_row,
            .ws_col = ws.ws_col,
            .ws_xpixel = ws.ws_xpixel,
            .ws_ypixel = ws.ws_ypixel,
        };
        if (guest_write(g, arg, &lws, sizeof(lws)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case LINUX_TCGETS: {
        /* Get terminal attributes.
         * Linux and macOS use different c_cc index assignments for control
         * characters (e.g., Linux VINTR=0, macOS VINTR=8). Must translate. */
        struct termios t;
        if (tcgetattr(host_fd, &t) < 0)
            return -LINUX_ENOTTY;
        /* macOS c_cc index → Linux c_cc index mapping.
         * Linux: VINTR=0 VQUIT=1 VERASE=2 VKILL=3 VEOF=4 VTIME=5
         *        VMIN=6 VSWTC=7 VSTART=8 VSTOP=9 VSUSP=10 VEOL=11
         *        VREPRINT=12 VDISCARD=13 VWERASE=14 VLNEXT=15 VEOL2=16
         * macOS: VEOF=0 VEOL=1 VEOL2=2 VERASE=3 VWERASE=4 VKILL=5
         *        VREPRINT=6 (7=spare) VINTR=8 VQUIT=9 VSUSP=10 VDSUSP=11
         *        VSTART=12 VSTOP=13 VLNEXT=14 VDISCARD=15 VMIN=16 VTIME=17 */
        static const int mac_to_linux_cc[19] = {
            /*[linux 0  VINTR]    = mac*/  8,
            /*[linux 1  VQUIT]    = mac*/  9,
            /*[linux 2  VERASE]   = mac*/  3,
            /*[linux 3  VKILL]    = mac*/  5,
            /*[linux 4  VEOF]     = mac*/  0,
            /*[linux 5  VTIME]    = mac*/ 17,
            /*[linux 6  VMIN]     = mac*/ 16,
            /*[linux 7  VSWTC]    = mac*/ -1, /* no macOS equivalent */
            /*[linux 8  VSTART]   = mac*/ 12,
            /*[linux 9  VSTOP]    = mac*/ 13,
            /*[linux 10 VSUSP]    = mac*/ 10,
            /*[linux 11 VEOL]     = mac*/  1,
            /*[linux 12 VREPRINT] = mac*/  6,
            /*[linux 13 VDISCARD] = mac*/ 15,
            /*[linux 14 VWERASE]  = mac*/  4,
            /*[linux 15 VLNEXT]   = mac*/ 14,
            /*[linux 16 VEOL2]    = mac*/  2,
            -1, -1, /* unused slots 17-18 */
        };
        linux_termios_t lt = {0};
        lt.c_iflag = (uint32_t)t.c_iflag;
        lt.c_oflag = (uint32_t)t.c_oflag;
        lt.c_cflag = (uint32_t)t.c_cflag;
        lt.c_lflag = (uint32_t)t.c_lflag;
        for (int i = 0; i < 19; i++) {
            int mac_idx = mac_to_linux_cc[i];
            lt.c_cc[i] = (mac_idx >= 0 && mac_idx < NCCS) ? t.c_cc[mac_idx] : 0;
        }
        lt.c_ispeed = (uint32_t)cfgetispeed(&t);
        lt.c_ospeed = (uint32_t)cfgetospeed(&t);
        if (guest_write(g, arg, &lt, sizeof(lt)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case LINUX_TCSETS:
    case LINUX_TCSETSW:
    case LINUX_TCSETSF: {
        /* Set terminal attributes with c_cc index translation (see TCGETS) */
        static const int linux_to_mac_cc[19] = {
            /*[linux 0  VINTR]    → mac*/  8,
            /*[linux 1  VQUIT]    → mac*/  9,
            /*[linux 2  VERASE]   → mac*/  3,
            /*[linux 3  VKILL]    → mac*/  5,
            /*[linux 4  VEOF]     → mac*/  0,
            /*[linux 5  VTIME]    → mac*/ 17,
            /*[linux 6  VMIN]     → mac*/ 16,
            /*[linux 7  VSWTC]    → mac*/ -1,
            /*[linux 8  VSTART]   → mac*/ 12,
            /*[linux 9  VSTOP]    → mac*/ 13,
            /*[linux 10 VSUSP]    → mac*/ 10,
            /*[linux 11 VEOL]     → mac*/  1,
            /*[linux 12 VREPRINT] → mac*/  6,
            /*[linux 13 VDISCARD] → mac*/ 15,
            /*[linux 14 VWERASE]  → mac*/  4,
            /*[linux 15 VLNEXT]   → mac*/ 14,
            /*[linux 16 VEOL2]    → mac*/  2,
            -1, -1,
        };
        linux_termios_t lt;
        if (guest_read(g, arg, &lt, sizeof(lt)) < 0)
            return -LINUX_EFAULT;
        struct termios t;
        if (tcgetattr(host_fd, &t) < 0)
            return -LINUX_ENOTTY;  /* Not a terminal */
        t.c_iflag = lt.c_iflag;
        t.c_oflag = lt.c_oflag;
        t.c_cflag = lt.c_cflag;
        t.c_lflag = lt.c_lflag;
        for (int i = 0; i < 19; i++) {
            int mac_idx = linux_to_mac_cc[i];
            if (mac_idx >= 0 && mac_idx < NCCS)
                t.c_cc[mac_idx] = lt.c_cc[i];
        }
        cfsetispeed(&t, lt.c_ispeed);
        cfsetospeed(&t, lt.c_ospeed);
        int action = (request == LINUX_TCSETSF) ? TCSAFLUSH
                   : (request == LINUX_TCSETSW) ? TCSADRAIN
                   : TCSANOW;
        if (tcsetattr(host_fd, action, &t) < 0)
            return linux_errno();
        return 0;
    }

    case LINUX_TIOCGPGRP: {
        /* Get foreground process group */
        pid_t pgrp = tcgetpgrp(host_fd);
        if (pgrp < 0) return -LINUX_ENOTTY;
        int32_t val = (int32_t)pgrp;
        if (guest_write(g, arg, &val, sizeof(val)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case LINUX_TIOCSPGRP: {
        /* Set foreground process group — stub (single-process model) */
        return 0;
    }

    case LINUX_TIOCSCTTY: {
        /* Acquire controlling terminal — stub */
        return 0;
    }

    case LINUX_TIOCNOTTY: {
        /* Release controlling terminal — stub */
        return 0;
    }

    case LINUX_FIONREAD: {
        /* Get bytes available for reading */
        int avail = 0;
        if (ioctl(host_fd, FIONREAD, &avail) < 0)
            return linux_errno();
        int32_t val = (int32_t)avail;
        if (guest_write(g, arg, &val, sizeof(val)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case LINUX_TIOCGSID: {
        /* Get session ID — return our PID (single-process model) */
        int32_t val = (int32_t)proc_get_pid();
        if (guest_write(g, arg, &val, sizeof(val)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    default:
        return -LINUX_ENOTTY;
    }
}

/* ---------- file space/copy ---------- */

int64_t sys_fallocate(int fd, int mode, int64_t offset, int64_t len) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux validates offset >= 0 and len > 0 */
    if (offset < 0 || len <= 0) return -LINUX_EINVAL;

    /* mode 0 = basic allocation → ftruncate fallback.
     * Other modes (FALLOC_FL_PUNCH_HOLE etc.) not supported. */
    if (mode != 0) return -LINUX_EOPNOTSUPP;

    struct stat st;
    if (fstat(host_fd, &st) < 0) return linux_errno();

    /* Extend file if needed (ftruncate only extends, doesn't shrink) */
    int64_t new_size = offset + len;
    if (new_size < offset) return -LINUX_EFBIG;  /* Overflow check */
    if (new_size > st.st_size) {
        if (ftruncate(host_fd, new_size) < 0)
            return linux_errno();
    }
    return 0;
}

int64_t sys_sendfile(guest_t *g, int out_fd, int in_fd,
                     uint64_t offset_gva, uint64_t count) {
    int host_out = fd_to_host(out_fd);
    int host_in = fd_to_host(in_fd);
    if (host_out < 0 || host_in < 0) return -LINUX_EBADF;

    /* macOS sendfile() requires a socket destination, so we emulate
     * with pread/write loop for general file-to-file copies. */
    int64_t offset = -1;
    if (offset_gva != 0) {
        if (guest_read(g, offset_gva, &offset, 8) < 0)
            return -LINUX_EFAULT;
    }

    char buf[65536];
    size_t total = 0;
    size_t remaining = count;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        ssize_t nr;
        if (offset >= 0) {
            nr = pread(host_in, buf, chunk, offset);
        } else {
            nr = read(host_in, buf, chunk);
        }
        if (nr <= 0) break;

        ssize_t nw = write(host_out, buf, nr);
        if (nw < 0) {
            if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
            if (total > 0) break;  /* Report partial success below */
            return linux_errno();
        }

        total += nw;
        remaining -= nw;
        if (offset >= 0) offset += nw;
        if (nw < nr) break;  /* Short write */
    }

    /* Write back updated offset (even on partial transfer).
     * Preserve partial success: if bytes were transferred but offset
     * writeback fails, return the count rather than -EFAULT. */
    if (offset_gva != 0) {
        if (guest_write(g, offset_gva, &offset, 8) < 0)
            return total > 0 ? (int64_t)total : -LINUX_EFAULT;
    }

    return (int64_t)total;
}

int64_t sys_copy_file_range(guest_t *g, int fd_in, uint64_t off_in_gva,
                            int fd_out, uint64_t off_out_gva,
                            uint64_t len, unsigned int flags) {
    (void)flags;
    int host_in = fd_to_host(fd_in);
    int host_out = fd_to_host(fd_out);
    if (host_in < 0 || host_out < 0) return -LINUX_EBADF;

    /* Read optional offsets from guest memory */
    int64_t off_in = -1, off_out = -1;
    if (off_in_gva != 0) {
        if (guest_read(g, off_in_gva, &off_in, 8) < 0)
            return -LINUX_EFAULT;
    }
    if (off_out_gva != 0) {
        if (guest_read(g, off_out_gva, &off_out, 8) < 0)
            return -LINUX_EFAULT;
    }

    /* Emulate with pread/pwrite loop */
    char buf[65536];
    size_t total = 0;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        ssize_t nr;
        if (off_in >= 0) {
            nr = pread(host_in, buf, chunk, off_in);
        } else {
            nr = read(host_in, buf, chunk);
        }
        if (nr <= 0) break;

        ssize_t nw;
        if (off_out >= 0) {
            nw = pwrite(host_out, buf, nr, off_out);
        } else {
            nw = write(host_out, buf, nr);
        }
        if (nw < 0) {
            if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
            if (total > 0) break;  /* Report partial success below */
            return linux_errno();
        }

        total += nw;
        remaining -= nw;
        if (off_in >= 0) off_in += nw;
        if (off_out >= 0) off_out += nw;
        if (nw < nr) break;
    }

    /* Write back updated offsets (even on partial transfer).
     * Preserve partial success on writeback failure. */
    if (off_in_gva != 0) {
        if (guest_write(g, off_in_gva, &off_in, 8) < 0)
            return total > 0 ? (int64_t)total : -LINUX_EFAULT;
    }
    if (off_out_gva != 0) {
        if (guest_write(g, off_out_gva, &off_out, 8) < 0)
            return total > 0 ? (int64_t)total : -LINUX_EFAULT;
    }

    return (int64_t)total;
}

/* ---------- splice/tee ---------- */

/* splice: emulate by reading from in_fd and writing to out_fd */
int64_t sys_splice(guest_t *g, int fd_in, uint64_t off_in_gva,
                   int fd_out, uint64_t off_out_gva,
                   size_t len, unsigned int flags) {
    (void)flags;
    int host_in = fd_to_host(fd_in);
    int host_out = fd_to_host(fd_out);
    if (host_in < 0 || host_out < 0) return -LINUX_EBADF;

    /* Handle offsets */
    int64_t off_in = -1, off_out = -1;
    if (off_in_gva) {
        if (guest_read(g, off_in_gva, &off_in, 8) < 0)
            return -LINUX_EFAULT;
    }
    if (off_out_gva) {
        if (guest_read(g, off_out_gva, &off_out, 8) < 0)
            return -LINUX_EFAULT;
    }

    /* Emulate with read/write loop using a host-side buffer */
    size_t chunk = len > 65536 ? 65536 : len;
    uint8_t *buf = malloc(chunk);
    if (!buf) return -LINUX_ENOMEM;

    size_t total = 0;
    int saved_errno = 0;  /* Preserve errno across free/guest_write */
    int rw_error = 0;     /* Track whether read or write failed */
    while (total < len) {
        size_t n = (len - total) > chunk ? chunk : (len - total);
        ssize_t r = (off_in >= 0) ? pread(host_in, buf, n, off_in)
                                  : read(host_in, buf, n);
        if (r < 0) { rw_error = 1; saved_errno = errno; break; }
        if (r == 0) break;  /* EOF */
        if (off_in >= 0) off_in += r;

        size_t written = 0;
        while (written < (size_t)r) {
            ssize_t w = (off_out >= 0)
                ? pwrite(host_out, buf + written, r - written, off_out)
                : write(host_out, buf + written, r - written);
            if (w <= 0) {
                if (w < 0) { rw_error = 1; saved_errno = errno; }
                if (w < 0 && saved_errno == EPIPE) signal_queue(LINUX_SIGPIPE);
                total += written;  /* Account for partial bytes written */
                goto done;
            }
            written += w;
            if (off_out >= 0) off_out += w;
        }
        total += r;
    }

done:
    free(buf);

    /* Write back updated offsets */
    if (off_in_gva && off_in >= 0 &&
        guest_write(g, off_in_gva, &off_in, 8) < 0)
        return -LINUX_EFAULT;
    if (off_out_gva && off_out >= 0 &&
        guest_write(g, off_out_gva, &off_out, 8) < 0)
        return -LINUX_EFAULT;

    /* Return bytes transferred, or errno only if read/write failed.
     * Restore saved_errno since free/guest_write may have clobbered it. */
    if (total > 0) return (int64_t)total;
    if (rw_error) { errno = saved_errno; return linux_errno(); }
    return 0;
}

/* vmsplice: emulate as writev to the pipe fd */
int64_t sys_vmsplice(guest_t *g, int fd, uint64_t iov_gva,
                     unsigned long nr_segs, unsigned int flags) {
    (void)flags;
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (nr_segs > 64) nr_segs = 64;

    size_t total = 0;
    for (unsigned long i = 0; i < nr_segs; i++) {
        linux_iovec_t liov;
        if (guest_read(g, iov_gva + i * sizeof(linux_iovec_t),
                       &liov, sizeof(liov)) < 0)
            return -LINUX_EFAULT;

        if (liov.iov_len == 0) continue;
        void *src = guest_ptr(g, liov.iov_base);
        if (!src) return total > 0 ? (int64_t)total : -LINUX_EFAULT;

        ssize_t w = write(host_fd, src, liov.iov_len);
        if (w < 0) {
            if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
            return total > 0 ? (int64_t)total : linux_errno();
        }
        total += w;
        if ((size_t)w < liov.iov_len) break;
    }

    return (int64_t)total;
}

/* tee: copy data between two pipes without consuming it.
 * Full emulation would need MSG_PEEK on pipe — just return -EINVAL
 * since it's rarely used. */
int64_t sys_tee(int fd_in, int fd_out, size_t len, unsigned int flags) {
    (void)fd_in; (void)fd_out; (void)len; (void)flags;
    return -LINUX_EINVAL;
}
