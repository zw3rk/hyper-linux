/* syscall_signal.h — Signal delivery for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux aarch64 signal structures and delivery API. Matches the kernel's
 * arch/arm64/kernel/signal.c:setup_rt_frame() layout so that musl's
 * __restore_rt → rt_sigreturn (SYS 139) can correctly restore state.
 */
#ifndef SYSCALL_SIGNAL_H
#define SYSCALL_SIGNAL_H

#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include <sys/time.h>
#include "guest.h"

/* ---------- Linux signal numbers (1-based, matching kernel) ---------- */
#define LINUX_SIGHUP     1
#define LINUX_SIGINT     2
#define LINUX_SIGQUIT    3
#define LINUX_SIGILL     4
#define LINUX_SIGTRAP    5
#define LINUX_SIGABRT    6
#define LINUX_SIGBUS     7
#define LINUX_SIGFPE     8
#define LINUX_SIGKILL    9
#define LINUX_SIGUSR1   10
#define LINUX_SIGSEGV   11
#define LINUX_SIGUSR2   12
#define LINUX_SIGPIPE   13
#define LINUX_SIGALRM   14
#define LINUX_SIGTERM   15
#define LINUX_SIGSTKFLT 16
#define LINUX_SIGCHLD   17
#define LINUX_SIGCONT   18
#define LINUX_SIGSTOP   19
#define LINUX_SIGTSTP   20
#define LINUX_SIGTTIN   21
#define LINUX_SIGTTOU   22
#define LINUX_SIGURG    23
#define LINUX_SIGXCPU   24
#define LINUX_SIGXFSZ   25
#define LINUX_SIGVTALRM 26
#define LINUX_SIGPROF   27
#define LINUX_SIGWINCH  28
#define LINUX_SIGIO     29
#define LINUX_SIGPWR    30
#define LINUX_SIGSYS    31

#define LINUX_SIGRTMIN  32
#define LINUX_NSIG      64

/* ---------- Linux sigaction flags ---------- */
#define LINUX_SA_NOCLDSTOP  0x00000001
#define LINUX_SA_NOCLDWAIT  0x00000002
#define LINUX_SA_SIGINFO    0x00000004
#define LINUX_SA_ONSTACK    0x08000000
#define LINUX_SA_RESTART    0x10000000
#define LINUX_SA_NODEFER    0x40000000
#define LINUX_SA_RESETHAND  0x80000000U
#define LINUX_SA_RESTORER   0x04000000

/* SIG_DFL and SIG_IGN as handler addresses */
#define LINUX_SIG_DFL  0ULL
#define LINUX_SIG_IGN  1ULL

/* ---------- Signal mask operations ---------- */
#define LINUX_SIG_BLOCK   0
#define LINUX_SIG_UNBLOCK 1
#define LINUX_SIG_SETMASK 2

/* ---------- Linux sigaction (kernel-style, aarch64) ---------- */
/* The kernel's struct sigaction for aarch64 (from include/uapi/asm-generic/signal.h):
 *   sa_handler or sa_sigaction  (8 bytes)
 *   sa_flags                    (8 bytes on LP64)
 *   sa_restorer                 (8 bytes)
 *   sa_mask                     (8 bytes — single uint64_t for 64 signals)
 */
typedef struct {
    uint64_t sa_handler;     /* Handler address (or SIG_DFL=0 / SIG_IGN=1) */
    uint64_t sa_flags;
    uint64_t sa_restorer;    /* __restore_rt trampoline (calls rt_sigreturn) */
    uint64_t sa_mask;        /* Blocked signals during handler execution */
} linux_sigaction_t;

/* ---------- Default signal dispositions ---------- */
typedef enum {
    SIG_DISP_TERM,    /* Terminate the process */
    SIG_DISP_IGN,     /* Ignore the signal */
    SIG_DISP_CORE,    /* Terminate with core dump (we just terminate) */
    SIG_DISP_STOP,    /* Stop the process (not supported, treat as ignore) */
    SIG_DISP_CONT,    /* Continue the process (not supported, treat as ignore) */
} sig_disposition_t;

/* ---------- Linux siginfo_t (aarch64, 128 bytes) ---------- */
typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t _pad0;
    /* Union payload — we only populate kill fields */
    int32_t si_pid;
    int32_t si_uid;
    uint8_t _pad[128 - 24];  /* Pad to 128 bytes total */
} linux_siginfo_t;

/* si_code values */
#define LINUX_SI_USER    0
#define LINUX_SI_KERNEL  128
#define LINUX_SI_TIMER   -2

/* ---------- Linux sigcontext (aarch64) ---------- */
/* From arch/arm64/include/uapi/asm/sigcontext.h */
typedef struct {
    uint64_t fault_address;
    uint64_t regs[31];       /* X0-X30 */
    uint64_t sp;             /* SP_EL0 */
    uint64_t pc;             /* ELR_EL1 at time of signal */
    uint64_t pstate;         /* SPSR_EL1 at time of signal */
    /* 8 bytes reserved for extensions (FPSIMD etc.) */
    uint8_t  __reserved[4096] __attribute__((aligned(16)));
} linux_sigcontext_t;

/* ---------- Linux stack_t ---------- */
typedef struct {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  _pad;
    uint64_t ss_size;
} linux_stack_t;

/* ---------- Linux ucontext_t (aarch64) ---------- */
typedef struct {
    uint64_t           uc_flags;
    uint64_t           uc_link;     /* Pointer to next ucontext (always 0) */
    linux_stack_t      uc_stack;
    uint64_t           uc_sigmask;  /* Signal mask to restore on sigreturn */
    uint8_t            _pad[128 - 8]; /* Pad __reserved in kernel to 1024 bits */
    linux_sigcontext_t uc_mcontext;
} linux_ucontext_t;

/* ---------- Linux rt_sigframe (pushed onto guest stack) ---------- */
/* From arch/arm64/kernel/signal.c — this is what setup_rt_frame() builds */
typedef struct {
    linux_siginfo_t  info;
    linux_ucontext_t uc;
} linux_rt_sigframe_t;

/* ---------- Signal state ---------- */
typedef struct {
    linux_sigaction_t actions[LINUX_NSIG];  /* Per-signal handler state */
    uint64_t          pending;              /* Bitmask of pending signals */
    uint64_t          blocked;              /* Bitmask of blocked signals */
    uint64_t          saved_blocked;        /* Original mask before sigsuspend */
    int               saved_blocked_valid;  /* Non-zero if saved_blocked is set */
} signal_state_t;

/* ---------- API ---------- */

/* Initialize signal state: all SIG_DFL, nothing pending/blocked. */
void signal_init(void);

/* Queue a signal for delivery. */
void signal_queue(int signum);

/* Consume (clear) a pending signal. Used by signalfd reads. */
void signal_consume(int signum);

/* Check if any unblocked signal is pending. */
int signal_pending(void);

/* Deliver the highest-priority pending unblocked signal to the guest.
 * Builds an rt_sigframe on the guest stack and redirects vCPU to handler.
 * Returns: 1 if signal was delivered, 0 if nothing pending,
 *         -1 if process should terminate (default TERM/CORE disposition).
 * On terminate, *exit_code is set to 128 + signum. */
int signal_deliver(hv_vcpu_t vcpu, guest_t *g, int *exit_code);

/* Handle rt_sigreturn (SYS 139): restore registers from rt_sigframe on
 * the guest stack. Returns SYSCALL_EXEC_HAPPENED to skip X0 writeback. */
int signal_rt_sigreturn(hv_vcpu_t vcpu, guest_t *g);

/* Handle rt_sigaction (SYS 134). */
int64_t signal_rt_sigaction(guest_t *g, int signum,
                            uint64_t act_gva, uint64_t oldact_gva,
                            uint64_t sigsetsize);

/* Handle rt_sigprocmask (SYS 135). */
int64_t signal_rt_sigprocmask(guest_t *g, int how,
                               uint64_t set_gva, uint64_t oldset_gva,
                               uint64_t sigsetsize);

/* Handle rt_sigsuspend (SYS 133). */
int64_t signal_rt_sigsuspend(guest_t *g, uint64_t mask_gva, uint64_t sigsetsize);

/* Handle rt_sigpending (SYS 136). */
int64_t signal_rt_sigpending(guest_t *g, uint64_t set_gva, uint64_t sigsetsize);

/* Get/set signal state (for fork IPC serialization). */
const signal_state_t *signal_get_state(void);
void signal_set_state(const signal_state_t *state);

/* Get default disposition for a signal. */
sig_disposition_t signal_default_disposition(int signum);

/* ---------- Guest ITIMER_REAL emulation ----------
 * These emulate the guest's setitimer(ITIMER_REAL) internally rather than
 * forwarding to the host, because macOS shares alarm() and setitimer() as
 * the same underlying timer — and hl needs alarm() for its vCPU timeout. */

/* Set the guest's ITIMER_REAL timer. value/interval are relative durations.
 * old_value/old_interval receive the previous timer state (may be NULL). */
void signal_set_itimer(const struct timeval *value,
                       const struct timeval *interval,
                       struct timeval *old_value,
                       struct timeval *old_interval);

/* Get the guest's ITIMER_REAL remaining time and interval. */
void signal_get_itimer(struct timeval *value, struct timeval *interval);

/* Check if the guest's ITIMER_REAL has expired; if so, queue SIGALRM.
 * Called from the vCPU loop after each syscall. */
void signal_check_timer(void);

#endif /* SYSCALL_SIGNAL_H */
