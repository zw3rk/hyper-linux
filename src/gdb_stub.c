/* gdb_stub.c — GDB Remote Serial Protocol stub for guest debugging
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements a minimal GDB RSP server over TCP. When the guest hits a
 * hardware breakpoint/watchpoint or receives Ctrl+C from GDB, all vCPUs
 * stop (all-stop mode) and the stub services register/memory queries.
 *
 * Architecture:
 *   - A listener thread accepts one GDB connection at a time
 *   - When a stop event occurs, the stopped thread notifies the stub
 *     via gdb_stub_handle_stop() which blocks until GDB resumes
 *   - All other threads are stopped via hv_vcpus_exit() (all-stop)
 *   - The stub thread reads GDB packets and responds synchronously
 *   - Hardware debug registers (DBGBVR/DBGBCR, DBGWVR/DBGWCR) are
 *     programmed on all vCPUs via gdb_stub_sync_debug_regs()
 *
 * Thread safety: the stub's internal state (breakpoint table, stop
 * state) is protected by gdb_lock. The vCPU threads call handle_stop
 * which blocks on gdb_resume_cond under gdb_lock.
 */
#include "gdb_stub.h"
#include "thread.h"
#include "hv_util.h"
#include "syscall.h"  /* linux_user_pt_regs_t, LINUX_* errno */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

/* ---------- Constants ---------- */

#define GDB_PKT_BUF_SIZE  (128 * 1024)  /* Max packet size (128KB for large memory reads) */
#define MAX_HW_BREAKPOINTS 16
#define MAX_HW_WATCHPOINTS 16

/* aarch64 DBGBCR enable bits:
 *   [0]     E     = 1 (enable)
 *   [2:1]   PMC   = 0b10 (EL0 only)
 *   [8:5]   BAS   = 0b1111 (match all bytes in word)
 *   [23:20] BT    = 0b0000 (unlinked address match)
 * Combined: E=1, PMC=0b10, BAS=0b1111 → 0x1E5
 * Wait, let me recalculate:
 *   bit 0: E=1
 *   bits [2:1]: PMC=0b10 → matches EL0 only
 *   bits [4:3]: 0b00 (reserved)
 *   bits [8:5]: BAS=0b1111 → 0xF << 5 = 0x1E0
 *   0x1E0 | 0x5 = 0x1E5
 * Actually:
 *   E=1: bit 0 = 1
 *   PMC=0b10: bits[2:1] = 10 → 0x4
 *   BAS=0b1111: bits[8:5] = 1111 → 0x1E0
 *   Total: 0x1E0 | 0x4 | 0x1 = 0x1E5 */
#define DBGBCR_ENABLE_EL0 0x1E5

/* aarch64 DBGWCR enable bits for watchpoints:
 *   [0]     E     = 1 (enable)
 *   [2:1]   PAC   = 0b10 (EL0 only)
 *   [4:3]   LSC   = depends on type (01=load, 10=store, 11=both)
 *   [12:5]  BAS   = byte address select (depends on length)
 *   [28:24] MASK  = 0b00000 (no address mask) */
#define DBGWCR_BASE  0x5  /* E=1, PAC=0b10 */
#define DBGWCR_LSC_STORE  (0x2 << 3)  /* Write watchpoint */
#define DBGWCR_LSC_LOAD   (0x1 << 3)  /* Read watchpoint */
#define DBGWCR_LSC_BOTH   (0x3 << 3)  /* Access watchpoint */

/* HVF debug register ID lookup tables.
 * The HVF enum values use stride 8 per index:
 *   BVR0=0x8004, BCR0=0x8005, BVR1=0x800c, BCR1=0x800d, etc.
 *   WVR0=0x8006, WCR0=0x8007, WVR1=0x800e, WCR1=0x800f, etc. */
static const hv_sys_reg_t dbgbvr_regs[MAX_HW_BREAKPOINTS] = {
    HV_SYS_REG_DBGBVR0_EL1,  HV_SYS_REG_DBGBVR1_EL1,
    HV_SYS_REG_DBGBVR2_EL1,  HV_SYS_REG_DBGBVR3_EL1,
    HV_SYS_REG_DBGBVR4_EL1,  HV_SYS_REG_DBGBVR5_EL1,
    HV_SYS_REG_DBGBVR6_EL1,  HV_SYS_REG_DBGBVR7_EL1,
    HV_SYS_REG_DBGBVR8_EL1,  HV_SYS_REG_DBGBVR9_EL1,
    HV_SYS_REG_DBGBVR10_EL1, HV_SYS_REG_DBGBVR11_EL1,
    HV_SYS_REG_DBGBVR12_EL1, HV_SYS_REG_DBGBVR13_EL1,
    HV_SYS_REG_DBGBVR14_EL1, HV_SYS_REG_DBGBVR15_EL1,
};
static const hv_sys_reg_t dbgbcr_regs[MAX_HW_BREAKPOINTS] = {
    HV_SYS_REG_DBGBCR0_EL1,  HV_SYS_REG_DBGBCR1_EL1,
    HV_SYS_REG_DBGBCR2_EL1,  HV_SYS_REG_DBGBCR3_EL1,
    HV_SYS_REG_DBGBCR4_EL1,  HV_SYS_REG_DBGBCR5_EL1,
    HV_SYS_REG_DBGBCR6_EL1,  HV_SYS_REG_DBGBCR7_EL1,
    HV_SYS_REG_DBGBCR8_EL1,  HV_SYS_REG_DBGBCR9_EL1,
    HV_SYS_REG_DBGBCR10_EL1, HV_SYS_REG_DBGBCR11_EL1,
    HV_SYS_REG_DBGBCR12_EL1, HV_SYS_REG_DBGBCR13_EL1,
    HV_SYS_REG_DBGBCR14_EL1, HV_SYS_REG_DBGBCR15_EL1,
};
static const hv_sys_reg_t dbgwvr_regs[MAX_HW_WATCHPOINTS] = {
    HV_SYS_REG_DBGWVR0_EL1,  HV_SYS_REG_DBGWVR1_EL1,
    HV_SYS_REG_DBGWVR2_EL1,  HV_SYS_REG_DBGWVR3_EL1,
    HV_SYS_REG_DBGWVR4_EL1,  HV_SYS_REG_DBGWVR5_EL1,
    HV_SYS_REG_DBGWVR6_EL1,  HV_SYS_REG_DBGWVR7_EL1,
    HV_SYS_REG_DBGWVR8_EL1,  HV_SYS_REG_DBGWVR9_EL1,
    HV_SYS_REG_DBGWVR10_EL1, HV_SYS_REG_DBGWVR11_EL1,
    HV_SYS_REG_DBGWVR12_EL1, HV_SYS_REG_DBGWVR13_EL1,
    HV_SYS_REG_DBGWVR14_EL1, HV_SYS_REG_DBGWVR15_EL1,
};
static const hv_sys_reg_t dbgwcr_regs[MAX_HW_WATCHPOINTS] = {
    HV_SYS_REG_DBGWCR0_EL1,  HV_SYS_REG_DBGWCR1_EL1,
    HV_SYS_REG_DBGWCR2_EL1,  HV_SYS_REG_DBGWCR3_EL1,
    HV_SYS_REG_DBGWCR4_EL1,  HV_SYS_REG_DBGWCR5_EL1,
    HV_SYS_REG_DBGWCR6_EL1,  HV_SYS_REG_DBGWCR7_EL1,
    HV_SYS_REG_DBGWCR8_EL1,  HV_SYS_REG_DBGWCR9_EL1,
    HV_SYS_REG_DBGWCR10_EL1, HV_SYS_REG_DBGWCR11_EL1,
    HV_SYS_REG_DBGWCR12_EL1, HV_SYS_REG_DBGWCR13_EL1,
    HV_SYS_REG_DBGWCR14_EL1, HV_SYS_REG_DBGWCR15_EL1,
};

/* ---------- Internal state ---------- */

/* Hardware breakpoint table entry */
typedef struct {
    uint64_t addr;  /* Virtual address */
    int      used;  /* Non-zero if slot is active */
    int      temp;  /* Non-zero if temporary (for single-step) */
} hw_bp_t;

/* Hardware watchpoint table entry */
typedef struct {
    uint64_t addr;  /* Virtual address */
    uint64_t len;   /* Byte length (1, 2, 4, or 8) */
    int      type;  /* 2=write, 3=read, 4=access (matches Z packet type) */
    int      used;  /* Non-zero if slot is active */
} hw_wp_t;

/* GDB stub global state, protected by gdb_lock */
static struct {
    int           initialized;
    int           listen_fd;          /* TCP listener socket */
    int           client_fd;          /* Connected GDB client (-1 if none) */
    guest_t      *guest;              /* Guest memory context */
    pthread_t     listener_thread;    /* Accepts connections + processes packets */
    pthread_mutex_t lock;             /* Protects all mutable state */
    pthread_cond_t  stop_cond;        /* Signaled when a thread stops */
    pthread_cond_t  resume_cond;      /* Signaled when GDB resumes threads */

    /* Stop state */
    int           all_stopped;        /* All threads are stopped */
    int64_t       stop_tid;           /* TID of thread that triggered the stop */
    int           stop_reason;        /* GDB_STOP_* value */
    uint64_t      stop_addr;          /* Address associated with stop (bp/wp addr) */
    int           resume_action;      /* 0=continue, 1=step (per stop_tid) */
    int           stop_requested;     /* GDB sent Ctrl+C or bp hit */

    /* Breakpoints and watchpoints */
    hw_bp_t       breakpoints[MAX_HW_BREAKPOINTS];
    hw_wp_t       watchpoints[MAX_HW_WATCHPOINTS];

    /* Thread tracking (which thread GDB is focused on) */
    int64_t       current_g_tid;      /* Thread for 'g'/'G' register ops */
    int64_t       current_c_tid;      /* Thread for 'c'/'s' continue/step */
} gdb = {
    .initialized = 0,
    .listen_fd   = -1,
    .client_fd   = -1,
    .lock        = PTHREAD_MUTEX_INITIALIZER,
};

/* ---------- Hex encoding helpers ---------- */

static const char hex_chars[] = "0123456789abcdef";

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Encode len bytes from src into hex string at dst (2*len chars + NUL).
 * Returns number of hex chars written. */
static int hex_encode(char *dst, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        dst[i * 2]     = hex_chars[(src[i] >> 4) & 0xF];
        dst[i * 2 + 1] = hex_chars[src[i] & 0xF];
    }
    dst[len * 2] = '\0';
    return (int)(len * 2);
}

/* Decode hex string at src (2*len chars) into len bytes at dst.
 * Returns number of bytes decoded, or -1 on invalid hex. */
static int hex_decode(uint8_t *dst, const char *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int hi = hex_val(src[i * 2]);
        int lo = hex_val(src[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)len;
}

/* Parse a hex number from string. Advances *pp past the number.
 * Returns the parsed value. */
static uint64_t parse_hex(const char **pp) {
    const char *p = *pp;
    uint64_t val = 0;
    while (1) {
        int d = hex_val(*p);
        if (d < 0) break;
        val = (val << 4) | (uint64_t)d;
        p++;
    }
    *pp = p;
    return val;
}

/* ---------- RSP packet I/O ---------- */

/* Compute GDB RSP checksum (sum of all chars mod 256). */
static uint8_t rsp_checksum(const char *data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += (uint8_t)data[i];
    return sum;
}

/* Send a raw RSP packet: $data#xx.
 * Returns 0 on success, -1 on failure. */
static int rsp_send(int fd, const char *data, size_t len) {
    uint8_t cksum = rsp_checksum(data, len);
    char hdr = '$';
    char trailer[3];
    trailer[0] = '#';
    trailer[1] = hex_chars[(cksum >> 4) & 0xF];
    trailer[2] = hex_chars[cksum & 0xF];

    /* Write header + data + trailer atomically (best effort) */
    struct iovec iov[3] = {
        { .iov_base = &hdr,      .iov_len = 1 },
        { .iov_base = (void *)data, .iov_len = len },
        { .iov_base = trailer,   .iov_len = 3 },
    };

    ssize_t total = (ssize_t)(1 + len + 3);
    ssize_t written = writev(fd, iov, 3);
    return (written == total) ? 0 : -1;
}

/* Send a simple string response. */
static int rsp_reply(const char *data) {
    if (gdb.client_fd < 0) return -1;
    return rsp_send(gdb.client_fd, data, strlen(data));
}

/* Send an empty response (unsupported packet). */
static int rsp_reply_empty(void) {
    return rsp_reply("");
}

/* Send an OK response. */
static int rsp_reply_ok(void) {
    return rsp_reply("OK");
}

/* Send an error response (Enn). */
static int rsp_reply_error(int errnum) {
    char buf[4];
    snprintf(buf, sizeof(buf), "E%02x", errnum & 0xFF);
    return rsp_reply(buf);
}

/* Read one RSP packet from the client. Strips $...# framing and
 * verifies checksum. Sends + acknowledgment on success.
 * Returns packet length, 0 on EOF, -1 on error.
 * Also handles bare 0x03 (Ctrl+C) by returning "\x03" as a 1-byte packet. */
static int rsp_recv(int fd, char *buf, size_t bufsz) {
    /* Read bytes until we get a complete packet or Ctrl+C */
    int state = 0;  /* 0=waiting for $, 1=reading data, 2=cksum1, 3=cksum2 */
    size_t pos = 0;
    char ck_hi = 0, ck_lo = 0;

    while (1) {
        uint8_t c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return (n == 0) ? 0 : -1;

        /* Acknowledge ack/nack from GDB (ignore) — only when waiting
         * for the next packet start ($). Inside packet data, + and -
         * are valid characters (e.g., qSupported:hwbreak+). */
        if (state == 0 && (c == '+' || c == '-')) continue;

        /* Ctrl+C: interrupt */
        if (c == 0x03) {
            buf[0] = 0x03;
            return 1;
        }

        switch (state) {
        case 0:
            if (c == '$') { state = 1; pos = 0; }
            break;
        case 1:
            if (c == '#') { state = 2; break; }
            if (pos < bufsz - 1) buf[pos++] = (char)c;
            break;
        case 2:
            ck_hi = (char)c;
            state = 3;
            break;
        case 3: {
            ck_lo = (char)c;
            buf[pos] = '\0';

            /* Verify checksum */
            uint8_t expected = (uint8_t)((hex_val(ck_hi) << 4) | hex_val(ck_lo));
            uint8_t actual = rsp_checksum(buf, pos);
            if (expected == actual) {
                /* Send ack */
                char ack = '+';
                (void)write(fd, &ack, 1);
                return (int)pos;
            } else {
                /* Send nack */
                char nak = '-';
                (void)write(fd, &nak, 1);
                state = 0;
                pos = 0;
            }
            break;
        }
        }
    }
}

/* ---------- Register snapshot ----------
 *
 * HVF requires vCPU register access on the owning thread. The GDB
 * handler runs on a separate thread, so we use a snapshot protocol:
 *
 * 1. When a vCPU thread stops for GDB (in gdb_stub_handle_stop),
 *    it calls snapshot_vcpu_regs() to copy HVF registers into its
 *    thread_entry_t.gdb_reg_snapshot buffer.
 * 2. The GDB handler thread reads/writes the snapshot directly.
 * 3. On resume, if gdb_regs_dirty is set, the vCPU thread calls
 *    restore_vcpu_regs() to apply changes back to HVF.
 *
 * GDB aarch64 register layout (target.xml order):
 *   X0-X30  (31 × 8 bytes = 248 bytes)     offset 0
 *   SP      (8 bytes)                       offset 248
 *   PC      (8 bytes)                       offset 256
 *   CPSR    (4 bytes)                       offset 264
 *   V0-V31  (32 × 16 bytes = 512 bytes)     offset 268
 *   FPSR    (4 bytes)                       offset 780
 *   FPCR    (4 bytes)                       offset 784
 * Total: 788 bytes → 1576 hex chars
 */

#define REG_OFF_GPR(n)  ((n) * 8)        /* X0-X30 */
#define REG_OFF_SP      (31 * 8)         /* 248 */
#define REG_OFF_PC      (32 * 8)         /* 256 */
#define REG_OFF_CPSR    (33 * 8)         /* 264 */
#define REG_OFF_V(n)    (268 + (n) * 16) /* V0-V31 */
#define REG_OFF_FPSR    (268 + 32 * 16)  /* 780 */
#define REG_OFF_FPCR    (784)            /* 784 */
#define REG_SNAPSHOT_SIZE 788

/* Snapshot vCPU registers into thread_entry_t.gdb_reg_snapshot.
 * MUST be called on the vCPU's owning thread. */
static void snapshot_vcpu_regs(thread_entry_t *t) {
    hv_vcpu_t vcpu = t->vcpu;
    uint8_t *s = t->gdb_reg_snapshot;
    memset(s, 0, REG_SNAPSHOT_SIZE);

    /* X0-X30 */
    for (int i = 0; i < 31; i++) {
        uint64_t val = vcpu_get_gpr(vcpu, (unsigned)i);
        memcpy(s + REG_OFF_GPR(i), &val, 8);
    }
    /* SP */
    uint64_t sp = vcpu_get_sysreg(vcpu, HV_SYS_REG_SP_EL0);
    memcpy(s + REG_OFF_SP, &sp, 8);
    /* PC */
    uint64_t pc = vcpu_get_sysreg(vcpu, HV_SYS_REG_ELR_EL1);
    memcpy(s + REG_OFF_PC, &pc, 8);
    /* CPSR (32-bit) */
    uint64_t pstate = vcpu_get_sysreg(vcpu, HV_SYS_REG_SPSR_EL1);
    uint32_t cpsr = (uint32_t)pstate;
    memcpy(s + REG_OFF_CPSR, &cpsr, 4);
    /* V0-V31 */
    for (int i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t val;
        HV_CHECK(hv_vcpu_get_simd_fp_reg(vcpu,
                 (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + i), &val));
        memcpy(s + REG_OFF_V(i), &val, 16);
    }
    /* FPSR */
    uint64_t fpsr;
    HV_CHECK(hv_vcpu_get_reg(vcpu, HV_REG_FPSR, &fpsr));
    uint32_t fpsr32 = (uint32_t)fpsr;
    memcpy(s + REG_OFF_FPSR, &fpsr32, 4);
    /* FPCR */
    uint64_t fpcr;
    HV_CHECK(hv_vcpu_get_reg(vcpu, HV_REG_FPCR, &fpcr));
    uint32_t fpcr32 = (uint32_t)fpcr;
    memcpy(s + REG_OFF_FPCR, &fpcr32, 4);

    t->gdb_regs_dirty = 0;
}

/* Restore registers from gdb_reg_snapshot back to the vCPU.
 * MUST be called on the vCPU's owning thread. Only called when
 * gdb_regs_dirty is set (GDB handler modified the snapshot). */
static void restore_vcpu_regs(thread_entry_t *t) {
    hv_vcpu_t vcpu = t->vcpu;
    const uint8_t *s = t->gdb_reg_snapshot;

    /* X0-X30 */
    for (int i = 0; i < 31; i++) {
        uint64_t val;
        memcpy(&val, s + REG_OFF_GPR(i), 8);
        vcpu_set_gpr(vcpu, (unsigned)i, val);
    }
    /* SP */
    uint64_t sp;
    memcpy(&sp, s + REG_OFF_SP, 8);
    vcpu_set_sysreg(vcpu, HV_SYS_REG_SP_EL0, sp);
    /* PC */
    uint64_t pc;
    memcpy(&pc, s + REG_OFF_PC, 8);
    vcpu_set_sysreg(vcpu, HV_SYS_REG_ELR_EL1, pc);
    /* CPSR */
    uint32_t cpsr;
    memcpy(&cpsr, s + REG_OFF_CPSR, 4);
    vcpu_set_sysreg(vcpu, HV_SYS_REG_SPSR_EL1, (uint64_t)cpsr);
    /* V0-V31 */
    for (int i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t val;
        memcpy(&val, s + REG_OFF_V(i), 16);
        HV_CHECK(hv_vcpu_set_simd_fp_reg(vcpu,
                 (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + i), val));
    }
    /* FPSR */
    uint32_t fpsr32;
    memcpy(&fpsr32, s + REG_OFF_FPSR, 4);
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_FPSR, (uint64_t)fpsr32));
    /* FPCR */
    uint32_t fpcr32;
    memcpy(&fpcr32, s + REG_OFF_FPCR, 4);
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_FPCR, (uint64_t)fpcr32));

    t->gdb_regs_dirty = 0;
}

/* ---------- Target description XML ----------
 *
 * Tells GDB the register layout so it knows how to interpret 'g'
 * responses. This is the aarch64 target description with GPRs,
 * SIMD/FP, and system registers. */

static const char target_xml[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "  <architecture>aarch64</architecture>\n"
    "  <osabi>GNU/Linux</osabi>\n"
    "  <feature name=\"org.gnu.gdb.aarch64.core\">\n"
    "    <reg name=\"x0\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x1\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x2\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x3\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x4\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x5\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x6\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x7\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x8\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x9\"  bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x10\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x11\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x12\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x13\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x14\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x15\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x16\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x17\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x18\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x19\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x20\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x21\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x22\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x23\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x24\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x25\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x26\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x27\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x28\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x29\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"x30\" bitsize=\"64\" type=\"uint64\"/>\n"
    "    <reg name=\"sp\"  bitsize=\"64\" type=\"data_ptr\"/>\n"
    "    <reg name=\"pc\"  bitsize=\"64\" type=\"code_ptr\"/>\n"
    "    <reg name=\"cpsr\" bitsize=\"32\" type=\"uint32\"/>\n"
    "  </feature>\n"
    "  <feature name=\"org.gnu.gdb.aarch64.fpu\">\n"
    "    <vector id=\"v2d\" type=\"ieee_double\" count=\"2\"/>\n"
    "    <vector id=\"v2u\" type=\"uint64\" count=\"2\"/>\n"
    "    <vector id=\"v2i\" type=\"int64\" count=\"2\"/>\n"
    "    <vector id=\"v4f\" type=\"ieee_single\" count=\"4\"/>\n"
    "    <vector id=\"v4u\" type=\"uint32\" count=\"4\"/>\n"
    "    <vector id=\"v4i\" type=\"int32\" count=\"4\"/>\n"
    "    <vector id=\"v8u\" type=\"uint16\" count=\"8\"/>\n"
    "    <vector id=\"v8i\" type=\"int16\" count=\"8\"/>\n"
    "    <vector id=\"v16u\" type=\"uint8\" count=\"16\"/>\n"
    "    <vector id=\"v16i\" type=\"int8\" count=\"16\"/>\n"
    "    <union id=\"vnd\">\n"
    "      <field name=\"f\" type=\"v2d\"/>\n"
    "      <field name=\"u\" type=\"v2u\"/>\n"
    "      <field name=\"s\" type=\"v2i\"/>\n"
    "    </union>\n"
    "    <union id=\"vns\">\n"
    "      <field name=\"f\" type=\"v4f\"/>\n"
    "      <field name=\"u\" type=\"v4u\"/>\n"
    "      <field name=\"s\" type=\"v4i\"/>\n"
    "    </union>\n"
    "    <union id=\"vnh\">\n"
    "      <field name=\"u\" type=\"v8u\"/>\n"
    "      <field name=\"s\" type=\"v8i\"/>\n"
    "    </union>\n"
    "    <union id=\"vnb\">\n"
    "      <field name=\"u\" type=\"v16u\"/>\n"
    "      <field name=\"s\" type=\"v16i\"/>\n"
    "    </union>\n"
    "    <union id=\"vnq\">\n"
    "      <field name=\"d\" type=\"vnd\"/>\n"
    "      <field name=\"s\" type=\"vns\"/>\n"
    "      <field name=\"h\" type=\"vnh\"/>\n"
    "      <field name=\"b\" type=\"vnb\"/>\n"
    "      <field name=\"q\" type=\"uint128\"/>\n"
    "    </union>\n";

/* Second half of target XML (split to stay under string literal limits) */
static const char target_xml_2[] =
    "    <reg name=\"v0\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v1\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v2\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v3\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v4\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v5\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v6\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v7\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v8\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v9\"  bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v10\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v11\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v12\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v13\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v14\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v15\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v16\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v17\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v18\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v19\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v20\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v21\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v22\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v23\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v24\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v25\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v26\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v27\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v28\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v29\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v30\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"v31\" bitsize=\"128\" type=\"vnq\"/>\n"
    "    <reg name=\"fpsr\" bitsize=\"32\" type=\"uint32\"/>\n"
    "    <reg name=\"fpcr\" bitsize=\"32\" type=\"uint32\"/>\n"
    "  </feature>\n"
    "</target>\n";

/* ---------- Breakpoint / watchpoint management ---------- */

/* BAS (Byte Address Select) for a watchpoint of given length aligned
 * to the watch address modulo 8. */
static uint32_t wp_bas_for_len(uint64_t addr, uint64_t len) {
    /* Watchpoint address must be doubleword-aligned in DBGWVR;
     * BAS selects which bytes within the doubleword to watch. */
    unsigned offset = (unsigned)(addr & 7);
    uint32_t mask = 0;
    for (unsigned i = 0; i < (unsigned)len && (offset + i) < 8; i++)
        mask |= (1U << (offset + i));
    return mask << 5;  /* BAS is bits [12:5] */
}

static int bp_insert(uint64_t addr) {
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (!gdb.breakpoints[i].used) {
            gdb.breakpoints[i].addr = addr;
            gdb.breakpoints[i].used = 1;
            gdb.breakpoints[i].temp = 0;
            return 0;
        }
    }
    return -1;  /* No free slots */
}

static int bp_remove(uint64_t addr) {
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (gdb.breakpoints[i].used && gdb.breakpoints[i].addr == addr) {
            gdb.breakpoints[i].used = 0;
            return 0;
        }
    }
    return -1;
}

static int wp_insert(uint64_t addr, uint64_t len, int type) {
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (!gdb.watchpoints[i].used) {
            gdb.watchpoints[i].addr = addr;
            gdb.watchpoints[i].len  = len;
            gdb.watchpoints[i].type = type;
            gdb.watchpoints[i].used = 1;
            return 0;
        }
    }
    return -1;
}

static int wp_remove(uint64_t addr, uint64_t len, int type) {
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (gdb.watchpoints[i].used &&
            gdb.watchpoints[i].addr == addr &&
            gdb.watchpoints[i].len == len &&
            gdb.watchpoints[i].type == type) {
            gdb.watchpoints[i].used = 0;
            return 0;
        }
    }
    return -1;
}

/* Remove all temporary breakpoints (used after single-step completes). */
static void bp_remove_temps(void) {
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (gdb.breakpoints[i].used && gdb.breakpoints[i].temp)
            gdb.breakpoints[i].used = 0;
    }
}

/* ---------- Debug register programming ---------- */

void gdb_stub_sync_debug_regs(hv_vcpu_t vcpu) {
    if (!gdb.initialized) return;

    /* Enable debug exceptions to trap to EL2 (host) */
    HV_CHECK(hv_vcpu_set_trap_debug_exceptions(vcpu, true));

    /* MDSCR_EL1: enable monitor debug (MDE, bit 15).
     * MDE is required for hardware breakpoint/watchpoint exceptions
     * to fire at EL0. */
    uint64_t mdscr;
    HV_CHECK(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, &mdscr));
    mdscr |= (1ULL << 15);  /* MDE */
    HV_CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, mdscr));

    /* Program hardware breakpoints using lookup tables */
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (gdb.breakpoints[i].used) {
            hv_return_t r1 = hv_vcpu_set_sys_reg(vcpu, dbgbvr_regs[i],
                                                  gdb.breakpoints[i].addr);
            hv_return_t r2 = hv_vcpu_set_sys_reg(vcpu, dbgbcr_regs[i],
                                                  DBGBCR_ENABLE_EL0);
            if (r1 != HV_SUCCESS || r2 != HV_SUCCESS) {
                /* Hardware may not support this many breakpoints — skip */
                gdb.breakpoints[i].used = 0;
            }
        } else {
            /* Disable this breakpoint slot (ignore errors for
             * unsupported hardware indices) */
            (void)hv_vcpu_set_sys_reg(vcpu, dbgbcr_regs[i], 0);
        }
    }

    /* Program hardware watchpoints */
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (gdb.watchpoints[i].used) {
            /* Align address to doubleword boundary for DBGWVR */
            uint64_t aligned = gdb.watchpoints[i].addr & ~7ULL;
            hv_return_t r1 = hv_vcpu_set_sys_reg(vcpu, dbgwvr_regs[i],
                                                  aligned);

            uint32_t wcr_val = DBGWCR_BASE;
            switch (gdb.watchpoints[i].type) {
            case 2: wcr_val |= DBGWCR_LSC_STORE; break;
            case 3: wcr_val |= DBGWCR_LSC_LOAD;  break;
            case 4: wcr_val |= DBGWCR_LSC_BOTH;  break;
            default: wcr_val |= DBGWCR_LSC_BOTH; break;
            }
            wcr_val |= wp_bas_for_len(gdb.watchpoints[i].addr,
                                       gdb.watchpoints[i].len);
            hv_return_t r2 = hv_vcpu_set_sys_reg(vcpu, dbgwcr_regs[i],
                                                  (uint64_t)wcr_val);
            if (r1 != HV_SUCCESS || r2 != HV_SUCCESS) {
                gdb.watchpoints[i].used = 0;
            }
        } else {
            (void)hv_vcpu_set_sys_reg(vcpu, dbgwcr_regs[i], 0);
        }
    }
}

/* ---------- Thread helpers ---------- */

/* Find the thread entry for a given guest TID. Returns NULL if not found.
 * Note: we return thread_entry_t* (not hv_vcpu_t) because valid vCPU
 * handles can be 0 (the first vCPU created by HVF). */
static thread_entry_t *find_thread_for_tid(int64_t tid) {
    return thread_find(tid);
}

/* Build the T05 stop reply with thread info. */
static void build_stop_reply(char *buf, size_t bufsz) {
    /* T05thread:XXXX; — SIGTRAP with thread ID */
    int64_t tid = gdb.stop_tid;
    if (tid <= 0) tid = 1;

    switch (gdb.stop_reason) {
    case GDB_STOP_WATCHPOINT:
        /* Report watchpoint with address: T05watch:ADDR;thread:TID; */
        snprintf(buf, bufsz, "T05watch:%llx;thread:%llx;",
                 (unsigned long long)gdb.stop_addr,
                 (unsigned long long)tid);
        break;
    case GDB_STOP_SIGNAL:
        /* Generic signal stop */
        snprintf(buf, bufsz, "T05thread:%llx;",
                 (unsigned long long)tid);
        break;
    default:
        /* Breakpoint, step, entry — all report as SIGTRAP */
        snprintf(buf, bufsz, "T05thread:%llx;",
                 (unsigned long long)tid);
        break;
    }
}

/* ---------- Packet handlers ---------- */

/* Return the snapshot offset and byte size for a GDB register number.
 * Returns 0 on success, -1 for invalid register. */
static int reg_offset_and_size(uint64_t regnum, int *out_off, int *out_size) {
    if (regnum < 31) {
        *out_off = REG_OFF_GPR((int)regnum);
        *out_size = 8;
    } else if (regnum == 31) {
        *out_off = REG_OFF_SP;
        *out_size = 8;
    } else if (regnum == 32) {
        *out_off = REG_OFF_PC;
        *out_size = 8;
    } else if (regnum == 33) {
        *out_off = REG_OFF_CPSR;
        *out_size = 4;
    } else if (regnum >= 34 && regnum < 66) {
        *out_off = REG_OFF_V((int)(regnum - 34));
        *out_size = 16;
    } else if (regnum == 66) {
        *out_off = REG_OFF_FPSR;
        *out_size = 4;
    } else if (regnum == 67) {
        *out_off = REG_OFF_FPCR;
        *out_size = 4;
    } else {
        return -1;
    }
    return 0;
}

/* Handle 'g' — read all registers from snapshot for current_g_tid. */
static void handle_read_regs(void) {
    thread_entry_t *t = find_thread_for_tid(gdb.current_g_tid);
    if (!t) {
        rsp_reply_error(1);
        return;
    }

    char hex_buf[REG_SNAPSHOT_SIZE * 2 + 1];
    hex_encode(hex_buf, t->gdb_reg_snapshot, REG_SNAPSHOT_SIZE);
    rsp_reply(hex_buf);
}

/* Handle 'G' — write all registers to snapshot for current_g_tid. */
static void handle_write_regs(const char *pkt) {
    thread_entry_t *t = find_thread_for_tid(gdb.current_g_tid);
    if (!t) {
        rsp_reply_error(1);
        return;
    }

    size_t hex_len = strlen(pkt);
    size_t decode_bytes = hex_len / 2;
    if (decode_bytes > REG_SNAPSHOT_SIZE) decode_bytes = REG_SNAPSHOT_SIZE;

    /* Pad with zeros if GDB sends fewer registers */
    if (decode_bytes < REG_SNAPSHOT_SIZE)
        memset(t->gdb_reg_snapshot + decode_bytes, 0,
               REG_SNAPSHOT_SIZE - decode_bytes);

    if (hex_decode(t->gdb_reg_snapshot, pkt, decode_bytes) < 0) {
        rsp_reply_error(1);
        return;
    }
    t->gdb_regs_dirty = 1;
    rsp_reply_ok();
}

/* Handle 'p' — read single register from snapshot. */
static void handle_read_reg(const char *pkt) {
    const char *p = pkt;
    uint64_t regnum = parse_hex(&p);

    thread_entry_t *t = find_thread_for_tid(gdb.current_g_tid);
    if (!t) {
        rsp_reply_error(1);
        return;
    }

    int off, nbytes;
    if (reg_offset_and_size(regnum, &off, &nbytes) < 0) {
        rsp_reply_error(14);  /* EFAULT — invalid register */
        return;
    }

    char hex_buf[64];
    hex_encode(hex_buf, t->gdb_reg_snapshot + off, (size_t)nbytes);
    rsp_reply(hex_buf);
}

/* Handle 'P' — write single register to snapshot. */
static void handle_write_reg(const char *pkt) {
    const char *p = pkt;
    uint64_t regnum = parse_hex(&p);
    if (*p == '=') p++;

    thread_entry_t *t = find_thread_for_tid(gdb.current_g_tid);
    if (!t) {
        rsp_reply_error(1);
        return;
    }

    int off, nbytes;
    if (reg_offset_and_size(regnum, &off, &nbytes) < 0) {
        rsp_reply_error(14);
        return;
    }

    uint8_t raw[16];
    size_t hex_len = strlen(p);
    size_t decode_len = hex_len / 2;
    if (decode_len > (size_t)nbytes) decode_len = (size_t)nbytes;
    if (hex_decode(raw, p, decode_len) < 0) {
        rsp_reply_error(1);
        return;
    }

    memcpy(t->gdb_reg_snapshot + off, raw, decode_len);
    t->gdb_regs_dirty = 1;
    rsp_reply_ok();
}

/* Handle 'm' — read memory: mADDR,LENGTH */
static void handle_read_mem(const char *pkt) {
    const char *p = pkt;
    uint64_t addr = parse_hex(&p);
    if (*p == ',') p++;
    uint64_t len = parse_hex(&p);

    if (len == 0) {
        rsp_reply_ok();
        return;
    }
    if (len > GDB_PKT_BUF_SIZE / 2 - 16) {
        rsp_reply_error(14);
        return;
    }

    /* Read from guest memory */
    uint8_t *tmp = malloc(len);
    if (!tmp) {
        rsp_reply_error(12);  /* ENOMEM */
        return;
    }

    if (guest_read(gdb.guest, addr, tmp, len) < 0) {
        free(tmp);
        rsp_reply_error(14);  /* EFAULT */
        return;
    }

    char *hex = malloc(len * 2 + 1);
    if (!hex) {
        free(tmp);
        rsp_reply_error(12);
        return;
    }
    hex_encode(hex, tmp, len);
    rsp_reply(hex);
    free(hex);
    free(tmp);
}

/* Handle 'M' — write memory: MADDR,LENGTH:XX... */
static void handle_write_mem(const char *pkt) {
    const char *p = pkt;
    uint64_t addr = parse_hex(&p);
    if (*p == ',') p++;
    uint64_t len = parse_hex(&p);
    if (*p == ':') p++;

    if (len == 0) {
        rsp_reply_ok();
        return;
    }

    uint8_t *tmp = malloc(len);
    if (!tmp) {
        rsp_reply_error(12);
        return;
    }

    if (hex_decode(tmp, p, len) < 0) {
        free(tmp);
        rsp_reply_error(1);
        return;
    }

    if (guest_write(gdb.guest, addr, tmp, len) < 0) {
        free(tmp);
        rsp_reply_error(14);
        return;
    }

    free(tmp);
    rsp_reply_ok();
}

/* Handle 'H' — set thread: HOP,THREADID */
static void handle_set_thread(const char *pkt) {
    char op = pkt[0];
    const char *p = pkt + 1;
    int64_t tid;
    int negative = 0;
    if (*p == '-') {
        negative = 1;
        p++;
    }
    tid = (int64_t)parse_hex(&p);
    if (negative) tid = -tid;

    /* tid 0 means "any thread" — pick the stop thread or first active */
    if (tid == 0 || tid == -1)
        tid = gdb.stop_tid > 0 ? gdb.stop_tid : 1;

    switch (op) {
    case 'g':
        gdb.current_g_tid = tid;
        break;
    case 'c':
        gdb.current_c_tid = tid;
        break;
    default:
        break;
    }
    rsp_reply_ok();
}

/* Handle 'T' — thread alive check: TTHREADID */
static void handle_thread_alive(const char *pkt) {
    const char *p = pkt;
    int64_t tid = (int64_t)parse_hex(&p);
    if (thread_tid_alive(tid))
        rsp_reply_ok();
    else
        rsp_reply_error(1);
}

/* Handle 'Z'/'z' — insert/remove breakpoint or watchpoint.
 * Format: Z/z TYPE,ADDR,KIND */
static void handle_breakpoint(const char *pkt, int insert) {
    const char *p = pkt;
    uint64_t type = parse_hex(&p);
    if (*p == ',') p++;
    uint64_t addr = parse_hex(&p);
    if (*p == ',') p++;
    uint64_t kind = parse_hex(&p);

    int rc;
    switch (type) {
    case 0:
        /* Software breakpoint — for MVP, treat as hardware breakpoint
         * since we have the HW support and this avoids I-cache issues */
        /* fallthrough */
    case 1:
        /* Hardware breakpoint */
        if (insert)
            rc = bp_insert(addr);
        else
            rc = bp_remove(addr);
        break;
    case 2: case 3: case 4:
        /* Watchpoint: 2=write, 3=read, 4=access */
        if (kind == 0) kind = 4;  /* Default 4-byte watch */
        if (insert)
            rc = wp_insert(addr, kind, (int)type);
        else
            rc = wp_remove(addr, kind, (int)type);
        break;
    default:
        rsp_reply_empty();  /* Unsupported type */
        return;
    }

    if (rc < 0)
        rsp_reply_error(28);  /* ENOSPC — no free HW slots */
    else
        rsp_reply_ok();
}

/* Handle qSupported — feature negotiation. */
static void handle_q_supported(const char *pkt) {
    (void)pkt;
    /* Advertise features:
     * - PacketSize: max packet we'll accept
     * - hwbreak+: we support hardware breakpoints
     * - swbreak+: we handle Z0 as hardware breakpoints
     * - qXfer:features:read+: we serve target.xml
     * - multiprocess-: no multiprocess support
     * - vContSupported+: we support vCont
     */
    char reply[256];
    snprintf(reply, sizeof(reply),
             "PacketSize=%x;hwbreak+;swbreak+;"
             "qXfer:features:read+;"
             "vContSupported+",
             GDB_PKT_BUF_SIZE);
    rsp_reply(reply);
}

/* Handle qXfer:features:read:target.xml:OFFSET,LENGTH */
static void handle_xfer_features(const char *pkt) {
    /* Parse annex:offset,length */
    const char *annex_start = pkt;
    const char *colon = strchr(pkt, ':');
    if (!colon) {
        rsp_reply_error(1);
        return;
    }

    /* Only support target.xml */
    size_t annex_len = (size_t)(colon - annex_start);
    if (annex_len != 10 || strncmp(annex_start, "target.xml", 10) != 0) {
        rsp_reply_error(1);
        return;
    }

    const char *p = colon + 1;
    uint64_t offset = parse_hex(&p);
    if (*p == ',') p++;
    uint64_t length = parse_hex(&p);

    /* Build full XML (concatenate two halves) */
    size_t xml1_len = strlen(target_xml);
    size_t xml2_len = strlen(target_xml_2);
    size_t total_len = xml1_len + xml2_len;

    if (offset >= total_len) {
        rsp_reply("l");  /* End of data */
        return;
    }

    /* Prepare the combined XML */
    char *full_xml = malloc(total_len + 1);
    if (!full_xml) {
        rsp_reply_error(12);
        return;
    }
    memcpy(full_xml, target_xml, xml1_len);
    memcpy(full_xml + xml1_len, target_xml_2, xml2_len);
    full_xml[total_len] = '\0';

    size_t remain = total_len - (size_t)offset;
    size_t chunk = remain < (size_t)length ? remain : (size_t)length;
    int last = (offset + chunk >= total_len);

    /* Build response: 'l' prefix if last chunk, 'm' if more */
    char *reply = malloc(chunk + 2);
    if (!reply) {
        free(full_xml);
        rsp_reply_error(12);
        return;
    }
    reply[0] = last ? 'l' : 'm';
    memcpy(reply + 1, full_xml + offset, chunk);
    reply[chunk + 1] = '\0';

    rsp_send(gdb.client_fd, reply, chunk + 1);
    free(reply);
    free(full_xml);
}

/* Callback for thread_for_each to collect TIDs into an array. */
typedef struct {
    int64_t tids[MAX_THREADS];
    int count;
} tid_collector_t;

static void collect_tids_cb(thread_entry_t *t, void *c) {
    tid_collector_t *cc = c;
    if (cc->count < MAX_THREADS)
        cc->tids[cc->count++] = t->guest_tid;
}

/* Handle qfThreadInfo / qsThreadInfo — list guest threads. */
static void handle_thread_info(int first) {
    if (!first) {
        rsp_reply("l");  /* End of list */
        return;
    }

    /* Build comma-separated hex thread ID list */
    char reply[1024];
    int pos = 0;
    reply[pos++] = 'm';

    /* Iterate through thread table */
    tid_collector_t ctx = { .count = 0 };
    thread_for_each(collect_tids_cb, &ctx);

    for (int i = 0; i < ctx.count; i++) {
        if (i > 0 && pos < (int)sizeof(reply) - 20)
            reply[pos++] = ',';
        pos += snprintf(reply + pos, sizeof(reply) - (size_t)pos,
                        "%llx", (unsigned long long)ctx.tids[i]);
    }
    reply[pos] = '\0';
    rsp_reply(reply);
}

/* Handle vCont — continue/step with per-thread control.
 * vCont;ACTION[:THREADID][;ACTION[:THREADID]]... */
static void handle_vcont(const char *pkt) {
    if (*pkt == '?') {
        /* vCont? — report supported actions */
        rsp_reply("vCont;c;C;s;S");
        return;
    }

    /* Parse actions — for MVP we support c (continue) and s (step).
     * In all-stop mode, all threads are resumed together. */
    int do_step = 0;
    int64_t step_tid = -1;  /* -1 = all threads */
    const char *p = pkt;

    while (*p == ';') {
        p++;
        char action = *p++;

        int64_t tid = -1;
        if (*p == ':') {
            p++;
            tid = (int64_t)parse_hex(&p);
        }

        switch (action) {
        case 's': case 'S':
            do_step = 1;
            step_tid = (tid > 0) ? tid : gdb.stop_tid;
            break;
        case 'c': case 'C':
            /* Continue — default action */
            break;
        default:
            break;
        }
    }

    /* Set up single-step if requested: place temporary HW breakpoint
     * at PC+4 (simple heuristic — works for non-branch instructions).
     * For branches, we'd need an instruction decoder. For MVP, PC+4
     * is sufficient for most cases; GDB handles the rest by setting
     * explicit breakpoints at branch targets. */
    if (do_step && step_tid > 0) {
        thread_entry_t *step_t = find_thread_for_tid(step_tid);
        if (step_t) {
            uint64_t pc;
            memcpy(&pc, step_t->gdb_reg_snapshot + REG_OFF_PC, 8);
            /* Set temporary breakpoint at next instruction */
            for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
                if (!gdb.breakpoints[i].used) {
                    gdb.breakpoints[i].addr = pc + 4;
                    gdb.breakpoints[i].used = 1;
                    gdb.breakpoints[i].temp = 1;
                    break;
                }
            }
        }
    }

    gdb.resume_action = do_step ? 1 : 0;

    /* Signal all stopped threads to resume */
    pthread_mutex_lock(&gdb.lock);
    gdb.all_stopped = 0;
    gdb.stop_requested = 0;
    pthread_cond_broadcast(&gdb.resume_cond);
    pthread_mutex_unlock(&gdb.lock);
}

/* Handle 'c' — continue execution. */
static void handle_continue(const char *pkt) {
    /* Optional address argument: cADDR — write to snapshot (applied on resume) */
    if (pkt[0] != '\0') {
        const char *p = pkt;
        uint64_t addr = parse_hex(&p);
        thread_entry_t *t = find_thread_for_tid(gdb.current_c_tid);
        if (t) {
            memcpy(t->gdb_reg_snapshot + REG_OFF_PC, &addr, 8);
            t->gdb_regs_dirty = 1;
        }
    }

    gdb.resume_action = 0;
    pthread_mutex_lock(&gdb.lock);
    gdb.all_stopped = 0;
    gdb.stop_requested = 0;
    pthread_cond_broadcast(&gdb.resume_cond);
    pthread_mutex_unlock(&gdb.lock);
}

/* Handle 's' — single-step. */
static void handle_step(const char *pkt) {
    /* Optional address argument — write to snapshot (applied on resume) */
    int64_t tid = gdb.current_c_tid;
    thread_entry_t *t = find_thread_for_tid(tid);
    if (pkt[0] != '\0' && t) {
        const char *p = pkt;
        uint64_t addr = parse_hex(&p);
        memcpy(t->gdb_reg_snapshot + REG_OFF_PC, &addr, 8);
        t->gdb_regs_dirty = 1;
    }

    /* Set temporary breakpoint at PC+4 (read from snapshot) */
    if (t) {
        uint64_t pc;
        memcpy(&pc, t->gdb_reg_snapshot + REG_OFF_PC, 8);
        for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
            if (!gdb.breakpoints[i].used) {
                gdb.breakpoints[i].addr = pc + 4;
                gdb.breakpoints[i].used = 1;
                gdb.breakpoints[i].temp = 1;
                break;
            }
        }
    }

    gdb.resume_action = 1;
    pthread_mutex_lock(&gdb.lock);
    gdb.all_stopped = 0;
    gdb.stop_requested = 0;
    pthread_cond_broadcast(&gdb.resume_cond);
    pthread_mutex_unlock(&gdb.lock);
}

/* Handle Ctrl+C (0x03) — interrupt all threads. */
static void handle_interrupt(void) {
    pthread_mutex_lock(&gdb.lock);
    gdb.stop_requested = 1;
    pthread_mutex_unlock(&gdb.lock);

    /* Force all vCPUs out of hv_vcpu_run */
    thread_interrupt_all();
}

/* Handle 'D' — detach. Disable all breakpoints and continue. */
static void handle_detach(void) {
    /* Clear all breakpoints and watchpoints */
    memset(gdb.breakpoints, 0, sizeof(gdb.breakpoints));
    memset(gdb.watchpoints, 0, sizeof(gdb.watchpoints));

    rsp_reply_ok();

    /* Resume all threads */
    pthread_mutex_lock(&gdb.lock);
    gdb.all_stopped = 0;
    gdb.stop_requested = 0;
    pthread_cond_broadcast(&gdb.resume_cond);
    pthread_mutex_unlock(&gdb.lock);

    /* Close client connection */
    close(gdb.client_fd);
    gdb.client_fd = -1;
}

/* Handle 'k' — kill. Terminate the guest. */
static void handle_kill(void) {
    /* Close connection and exit process */
    if (gdb.client_fd >= 0) {
        close(gdb.client_fd);
        gdb.client_fd = -1;
    }
    fprintf(stderr, "hl: GDB kill request — exiting\n");
    exit(0);
}

/* ---------- Main packet dispatch ---------- */

static void handle_packet(const char *pkt, int pkt_len) {
    if (pkt_len == 0) return;

    /* Ctrl+C */
    if (pkt[0] == 0x03) {
        handle_interrupt();
        return;
    }

    switch (pkt[0]) {
    case '?':
        /* Stop reason query */
        {
            char reply[128];
            build_stop_reply(reply, sizeof(reply));
            rsp_reply(reply);
        }
        break;

    case 'g':
        handle_read_regs();
        break;

    case 'G':
        handle_write_regs(pkt + 1);
        break;

    case 'p':
        handle_read_reg(pkt + 1);
        break;

    case 'P':
        handle_write_reg(pkt + 1);
        break;

    case 'm':
        handle_read_mem(pkt + 1);
        break;

    case 'M':
        handle_write_mem(pkt + 1);
        break;

    case 'c':
        handle_continue(pkt + 1);
        break;

    case 's':
        handle_step(pkt + 1);
        break;

    case 'H':
        handle_set_thread(pkt + 1);
        break;

    case 'T':
        handle_thread_alive(pkt + 1);
        break;

    case 'Z':
        handle_breakpoint(pkt + 1, 1);
        break;

    case 'z':
        handle_breakpoint(pkt + 1, 0);
        break;

    case 'D':
        handle_detach();
        break;

    case 'k':
        handle_kill();
        break;

    case 'v':
        if (strncmp(pkt, "vCont", 5) == 0) {
            handle_vcont(pkt + 5);
        } else if (strncmp(pkt, "vMustReplyEmpty", 15) == 0) {
            rsp_reply_empty();
        } else {
            rsp_reply_empty();
        }
        break;

    case 'q':
        if (strncmp(pkt, "qSupported", 10) == 0) {
            handle_q_supported(pkt + 10);
        } else if (strcmp(pkt, "qfThreadInfo") == 0) {
            handle_thread_info(1);
        } else if (strcmp(pkt, "qsThreadInfo") == 0) {
            handle_thread_info(0);
        } else if (strncmp(pkt, "qXfer:features:read:", 20) == 0) {
            handle_xfer_features(pkt + 20);
        } else if (strcmp(pkt, "qAttached") == 0) {
            rsp_reply("1");  /* Attached to existing process */
        } else if (strncmp(pkt, "qC", 2) == 0) {
            /* Current thread */
            char reply[32];
            snprintf(reply, sizeof(reply), "QC%llx",
                     (unsigned long long)(gdb.stop_tid > 0 ? gdb.stop_tid : 1));
            rsp_reply(reply);
        } else if (strcmp(pkt, "qOffsets") == 0) {
            rsp_reply("Text=0;Data=0;Bss=0");
        } else if (strcmp(pkt, "qSymbol::") == 0) {
            rsp_reply_ok();
        } else {
            rsp_reply_empty();
        }
        break;

    case 'Q':
        /* Set commands — mostly unsupported */
        rsp_reply_empty();
        break;

    default:
        rsp_reply_empty();
        break;
    }
}

/* ---------- GDB client session ---------- */

/* Main loop for servicing a connected GDB client. Runs on the listener
 * thread. Reads packets and dispatches them. Returns when the client
 * disconnects. */
static void gdb_client_session(void) {
    char *pkt_buf = malloc(GDB_PKT_BUF_SIZE);
    if (!pkt_buf) {
        fprintf(stderr, "hl: gdb: failed to allocate packet buffer\n");
        return;
    }

    while (gdb.client_fd >= 0) {
        /* Wait for either a packet from GDB or a stop event from a vCPU.
         * Use poll() so we can wake up when a thread stops. */
        struct pollfd pfd = {
            .fd     = gdb.client_fd,
            .events = POLLIN,
        };

        int pr = poll(&pfd, 1, -1);
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR) continue;
            break;
        }

        if (pfd.revents & (POLLERR | POLLHUP)) break;

        int pkt_len = rsp_recv(gdb.client_fd, pkt_buf, GDB_PKT_BUF_SIZE);
        if (pkt_len <= 0) break;

        handle_packet(pkt_buf, pkt_len);
    }

    free(pkt_buf);
}

/* ---------- Listener thread ---------- */

static void *listener_thread_fn(void *arg) {
    (void)arg;
    pthread_setname_np("gdb-stub");

    while (gdb.listen_fd >= 0) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int fd = accept(gdb.listen_fd, (struct sockaddr *)&client_addr,
                        &addr_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Disable Nagle's algorithm for responsive packet exchange */
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        fprintf(stderr, "hl: GDB client connected from %s:%d\n",
                inet_ntoa(client_addr.sin_addr),
                ntohs(client_addr.sin_port));

        gdb.client_fd = fd;

        /* If we're in stop-on-entry mode, the main thread is already
         * blocked in gdb_stub_wait_for_attach(). Wake it up now that
         * we have a client. */
        pthread_mutex_lock(&gdb.lock);
        pthread_cond_broadcast(&gdb.stop_cond);
        pthread_mutex_unlock(&gdb.lock);

        gdb_client_session();

        if (gdb.client_fd >= 0) {
            close(gdb.client_fd);
            gdb.client_fd = -1;
        }

        fprintf(stderr, "hl: GDB client disconnected\n");

        /* Resume all threads if client disconnects while stopped */
        pthread_mutex_lock(&gdb.lock);
        if (gdb.all_stopped) {
            gdb.all_stopped = 0;
            gdb.stop_requested = 0;
            memset(gdb.breakpoints, 0, sizeof(gdb.breakpoints));
            memset(gdb.watchpoints, 0, sizeof(gdb.watchpoints));
            pthread_cond_broadcast(&gdb.resume_cond);
        }
        pthread_mutex_unlock(&gdb.lock);
    }

    return NULL;
}

/* ---------- Public API ---------- */

int gdb_stub_init(int port, guest_t *g) {
    if (gdb.initialized) return 0;

    gdb.guest = g;
    gdb.current_g_tid = 1;
    gdb.current_c_tid = 1;
    pthread_cond_init(&gdb.stop_cond, NULL);
    pthread_cond_init(&gdb.resume_cond, NULL);

    /* Create TCP listener socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("hl: gdb: socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
        .sin_addr   = { .s_addr = htonl(INADDR_LOOPBACK) },
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("hl: gdb: bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        perror("hl: gdb: listen");
        close(fd);
        return -1;
    }

    gdb.listen_fd = fd;
    gdb.initialized = 1;

    fprintf(stderr, "hl: GDB stub listening on localhost:%d\n", port);
    fprintf(stderr, "hl: Connect with: aarch64-linux-gnu-gdb -ex "
            "\"target remote :%d\" <binary>\n", port);

    /* Start listener thread */
    if (pthread_create(&gdb.listener_thread, NULL, listener_thread_fn, NULL) != 0) {
        perror("hl: gdb: pthread_create");
        close(fd);
        gdb.listen_fd = -1;
        gdb.initialized = 0;
        return -1;
    }

    return 0;
}

void gdb_stub_wait_for_attach(void) {
    if (!gdb.initialized) return;

    fprintf(stderr, "hl: Waiting for GDB to attach...\n");

    /* Snapshot registers before blocking — runs on the vCPU owner thread.
     * This lets GDB inspect initial register state at entry. */
    if (current_thread)
        snapshot_vcpu_regs(current_thread);

    pthread_mutex_lock(&gdb.lock);

    /* Wait until a client connects */
    while (gdb.client_fd < 0)
        pthread_cond_wait(&gdb.stop_cond, &gdb.lock);

    /* Enter stopped state so GDB can inspect initial state */
    gdb.all_stopped = 1;
    gdb.stop_tid = current_thread ? current_thread->guest_tid : 1;
    gdb.stop_reason = GDB_STOP_ENTRY;
    gdb.stop_addr = 0;
    gdb.current_g_tid = gdb.stop_tid;
    gdb.current_c_tid = gdb.stop_tid;

    /* Block until GDB resumes us (via 'c' or 's') */
    while (gdb.all_stopped)
        pthread_cond_wait(&gdb.resume_cond, &gdb.lock);

    pthread_mutex_unlock(&gdb.lock);

    /* Apply any register changes GDB made */
    if (current_thread && current_thread->gdb_regs_dirty)
        restore_vcpu_regs(current_thread);

    fprintf(stderr, "hl: GDB attached, starting guest\n");
}

int gdb_stub_is_active(void) {
    return gdb.initialized && gdb.client_fd >= 0;
}

int gdb_stub_handle_stop(int stop_reason, uint64_t stop_addr) {
    if (!gdb.initialized || gdb.client_fd < 0) return 0;

    /* Remove temporary breakpoints after step completes */
    bp_remove_temps();

    int64_t my_tid = current_thread ? current_thread->guest_tid : 1;

    /* Snapshot vCPU registers into thread entry — must happen on the
     * vCPU's owning thread (HVF requirement). The GDB handler thread
     * reads/writes this snapshot while we're blocked. */
    if (current_thread)
        snapshot_vcpu_regs(current_thread);

    pthread_mutex_lock(&gdb.lock);

    /* Record stop info */
    gdb.all_stopped = 1;
    gdb.stop_tid = my_tid;
    gdb.stop_reason = stop_reason;
    gdb.stop_addr = stop_addr;
    gdb.stop_requested = 0;

    /* Update focus thread */
    gdb.current_g_tid = my_tid;
    gdb.current_c_tid = my_tid;

    pthread_mutex_unlock(&gdb.lock);

    /* Stop all other vCPUs (all-stop mode) */
    thread_interrupt_all();

    /* Send stop reply to GDB */
    char reply[128];
    build_stop_reply(reply, sizeof(reply));
    rsp_reply(reply);

    /* Block this thread until GDB resumes */
    pthread_mutex_lock(&gdb.lock);
    while (gdb.all_stopped)
        pthread_cond_wait(&gdb.resume_cond, &gdb.lock);

    int do_step = gdb.resume_action;
    pthread_mutex_unlock(&gdb.lock);

    /* Apply any register changes GDB made to the snapshot */
    if (current_thread && current_thread->gdb_regs_dirty)
        restore_vcpu_regs(current_thread);

    /* Re-sync debug registers before resuming */
    if (current_thread)
        gdb_stub_sync_debug_regs(current_thread->vcpu);

    return do_step;
}

int gdb_stub_stop_requested(void) {
    if (!gdb.initialized) return 0;
    return __atomic_load_n(&gdb.stop_requested, __ATOMIC_ACQUIRE);
}

void gdb_stub_notify_thread_created(int64_t tid) {
    /* In all-stop mode, GDB discovers threads via qfThreadInfo.
     * No notification packet needed. */
    (void)tid;
}

void gdb_stub_notify_thread_exited(int64_t tid) {
    (void)tid;
}

void gdb_stub_shutdown(void) {
    if (!gdb.initialized) return;

    /* Close listener socket to break accept() in listener thread */
    if (gdb.listen_fd >= 0) {
        close(gdb.listen_fd);
        gdb.listen_fd = -1;
    }

    /* Close client if connected */
    if (gdb.client_fd >= 0) {
        close(gdb.client_fd);
        gdb.client_fd = -1;
    }

    /* Resume any stopped threads so they can exit */
    pthread_mutex_lock(&gdb.lock);
    gdb.all_stopped = 0;
    gdb.stop_requested = 0;
    pthread_cond_broadcast(&gdb.resume_cond);
    pthread_mutex_unlock(&gdb.lock);

    gdb.initialized = 0;
}
