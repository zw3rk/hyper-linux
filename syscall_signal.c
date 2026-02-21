/* syscall_signal.c — Signal delivery for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements Linux-compatible signal delivery for aarch64 guests. When a
 * signal is queued (e.g., SIGPIPE from write() to broken pipe), we build
 * an rt_sigframe on the guest stack matching the kernel's setup_rt_frame()
 * layout, then redirect the vCPU to the guest's signal handler. The guest
 * handler eventually calls rt_sigreturn (SYS 139), which restores the
 * saved register state from the frame.
 *
 * Reference: Linux arch/arm64/kernel/signal.c
 */
#include "syscall_signal.h"
#include "syscall.h"        /* LINUX_E* errno constants */
#include "syscall_proc.h"   /* proc_get_pid, SYSCALL_EXEC_HAPPENED */

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>

/* ---------- Signal state (module-level, process-wide) ---------- */
static signal_state_t sig_state;

/* Protects signal actions array. Multiple threads may call rt_sigaction
 * concurrently (e.g., during musl init). Pending/blocked masks are
 * process-wide for the MVP; per-thread masks are deferred. */
static pthread_mutex_t sig_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Guest ITIMER_REAL emulation ----------
 * We emulate the guest's ITIMER_REAL internally rather than forwarding to
 * the host setitimer(), because macOS shares alarm() and setitimer(ITIMER_REAL)
 * as the same underlying timer — and hl needs alarm() for its own vCPU
 * per-iteration timeout. The guest timer is checked after each syscall in
 * the vCPU loop via signal_check_timer(). */
static struct {
    int active;                 /* Non-zero if timer is armed */
    struct timeval expiry;      /* Absolute wall-clock time of next fire */
    struct timeval interval;    /* Repeat interval (zero = one-shot) */
} guest_itimer;

/* ---------- Default disposition table ---------- */
/* Index 0 unused (signals are 1-based). */
static const sig_disposition_t default_dispositions[LINUX_NSIG + 1] = {
    [0]               = SIG_DISP_IGN,   /* Invalid signal 0 */
    [LINUX_SIGHUP]    = SIG_DISP_TERM,
    [LINUX_SIGINT]    = SIG_DISP_TERM,
    [LINUX_SIGQUIT]   = SIG_DISP_CORE,
    [LINUX_SIGILL]    = SIG_DISP_CORE,
    [LINUX_SIGTRAP]   = SIG_DISP_CORE,
    [LINUX_SIGABRT]   = SIG_DISP_CORE,
    [LINUX_SIGBUS]    = SIG_DISP_CORE,
    [LINUX_SIGFPE]    = SIG_DISP_CORE,
    [LINUX_SIGKILL]   = SIG_DISP_TERM,  /* Cannot be caught */
    [LINUX_SIGUSR1]   = SIG_DISP_TERM,
    [LINUX_SIGSEGV]   = SIG_DISP_CORE,
    [LINUX_SIGUSR2]   = SIG_DISP_TERM,
    [LINUX_SIGPIPE]   = SIG_DISP_TERM,
    [LINUX_SIGALRM]   = SIG_DISP_TERM,
    [LINUX_SIGTERM]   = SIG_DISP_TERM,
    [LINUX_SIGSTKFLT] = SIG_DISP_TERM,
    [LINUX_SIGCHLD]   = SIG_DISP_IGN,
    [LINUX_SIGCONT]   = SIG_DISP_CONT,
    [LINUX_SIGSTOP]   = SIG_DISP_STOP,  /* Cannot be caught */
    [LINUX_SIGTSTP]   = SIG_DISP_STOP,
    [LINUX_SIGTTIN]   = SIG_DISP_STOP,
    [LINUX_SIGTTOU]   = SIG_DISP_STOP,
    [LINUX_SIGURG]    = SIG_DISP_IGN,
    [LINUX_SIGXCPU]   = SIG_DISP_CORE,
    [LINUX_SIGXFSZ]   = SIG_DISP_CORE,
    [LINUX_SIGVTALRM] = SIG_DISP_TERM,
    [LINUX_SIGPROF]   = SIG_DISP_TERM,
    [LINUX_SIGWINCH]  = SIG_DISP_IGN,
    [LINUX_SIGIO]     = SIG_DISP_TERM,
    [LINUX_SIGPWR]    = SIG_DISP_TERM,
    [LINUX_SIGSYS]    = SIG_DISP_CORE,
    /* 32-64 (RT signals): default TERM */
};

sig_disposition_t signal_default_disposition(int signum) {
    if (signum < 1 || signum > LINUX_NSIG) return SIG_DISP_IGN;
    if (signum >= LINUX_SIGRTMIN) return SIG_DISP_TERM;
    return default_dispositions[signum];
}

/* ---------- Helpers ---------- */

/* Convert signal number (1-based) to bitmask position. */
static inline uint64_t sig_bit(int signum) {
    if (signum < 1 || signum > LINUX_NSIG) return 0;
    return 1ULL << (signum - 1);
}

/* Signals that cannot be caught, blocked, or ignored. */
static inline int sig_uncatchable(int signum) {
    return signum == LINUX_SIGKILL || signum == LINUX_SIGSTOP;
}

/* ---------- Public API ---------- */

void signal_init(void) {
    memset(&sig_state, 0, sizeof(sig_state));
}

void signal_queue(int signum) {
    if (signum < 1 || signum > LINUX_NSIG) return;
    sig_state.pending |= sig_bit(signum);
}

void signal_consume(int signum) {
    if (signum < 1 || signum > LINUX_NSIG) return;
    sig_state.pending &= ~sig_bit(signum);
}

int signal_pending(void) {
    return (sig_state.pending & ~sig_state.blocked) != 0;
}

const signal_state_t *signal_get_state(void) {
    return &sig_state;
}

void signal_set_state(const signal_state_t *state) {
    if (state) sig_state = *state;
}

/* ---------- Guest ITIMER_REAL API ---------- */

/* Helper: compare timevals. Returns <0, 0, >0. */
static int timeval_cmp(const struct timeval *a, const struct timeval *b) {
    if (a->tv_sec != b->tv_sec) return (a->tv_sec < b->tv_sec) ? -1 : 1;
    if (a->tv_usec != b->tv_usec) return (a->tv_usec < b->tv_usec) ? -1 : 1;
    return 0;
}

/* Helper: add two timevals. */
static struct timeval timeval_add(const struct timeval *a,
                                  const struct timeval *b) {
    struct timeval r = {
        .tv_sec  = a->tv_sec + b->tv_sec,
        .tv_usec = a->tv_usec + b->tv_usec,
    };
    if (r.tv_usec >= 1000000) {
        r.tv_sec += 1;
        r.tv_usec -= 1000000;
    }
    return r;
}

/* Helper: subtract b from a (a must be >= b). */
static struct timeval timeval_sub(const struct timeval *a,
                                  const struct timeval *b) {
    struct timeval r = {
        .tv_sec  = a->tv_sec - b->tv_sec,
        .tv_usec = a->tv_usec - b->tv_usec,
    };
    if (r.tv_usec < 0) {
        r.tv_sec -= 1;
        r.tv_usec += 1000000;
    }
    return r;
}

void signal_set_itimer(const struct timeval *value,
                       const struct timeval *interval,
                       struct timeval *old_value,
                       struct timeval *old_interval) {
    struct timeval now;
    gettimeofday(&now, NULL);

    /* Return old timer state */
    if (old_interval) *old_interval = guest_itimer.interval;
    if (old_value) {
        if (guest_itimer.active && timeval_cmp(&guest_itimer.expiry, &now) > 0) {
            *old_value = timeval_sub(&guest_itimer.expiry, &now);
        } else {
            old_value->tv_sec = 0;
            old_value->tv_usec = 0;
        }
    }

    /* Set new timer */
    if (value->tv_sec == 0 && value->tv_usec == 0) {
        /* Disarm */
        guest_itimer.active = 0;
    } else {
        guest_itimer.active = 1;
        guest_itimer.expiry = timeval_add(&now, value);
        guest_itimer.interval = interval ? *interval : (struct timeval){0, 0};
    }
}

void signal_get_itimer(struct timeval *value, struct timeval *interval) {
    if (interval) *interval = guest_itimer.interval;
    if (value) {
        if (guest_itimer.active) {
            struct timeval now;
            gettimeofday(&now, NULL);
            if (timeval_cmp(&guest_itimer.expiry, &now) > 0) {
                *value = timeval_sub(&guest_itimer.expiry, &now);
            } else {
                value->tv_sec = 0;
                value->tv_usec = 0;
            }
        } else {
            value->tv_sec = 0;
            value->tv_usec = 0;
        }
    }
}

void signal_check_timer(void) {
    if (!guest_itimer.active) return;

    struct timeval now;
    gettimeofday(&now, NULL);

    if (timeval_cmp(&now, &guest_itimer.expiry) >= 0) {
        /* Timer fired — queue SIGALRM */
        signal_queue(LINUX_SIGALRM);

        if (guest_itimer.interval.tv_sec != 0 ||
            guest_itimer.interval.tv_usec != 0) {
            /* Repeating timer: compute next expiry */
            guest_itimer.expiry = timeval_add(&now, &guest_itimer.interval);
        } else {
            /* One-shot: disarm */
            guest_itimer.active = 0;
        }
    }
}

/* ---------- rt_sigaction ---------- */

int64_t signal_rt_sigaction(guest_t *g, int signum,
                            uint64_t act_gva, uint64_t oldact_gva,
                            uint64_t sigsetsize) {
    if (sigsetsize != 8) return -LINUX_EINVAL;
    if (signum < 1 || signum > LINUX_NSIG) return -LINUX_EINVAL;
    if (sig_uncatchable(signum)) return -LINUX_EINVAL;

    int idx = signum - 1;

    pthread_mutex_lock(&sig_lock);

    /* Return old action if requested */
    if (oldact_gva) {
        if (guest_write(g, oldact_gva, &sig_state.actions[idx],
                        sizeof(linux_sigaction_t)) < 0) {
            pthread_mutex_unlock(&sig_lock);
            return -LINUX_EFAULT;
        }
    }

    /* Install new action if provided */
    if (act_gva) {
        linux_sigaction_t act;
        if (guest_read(g, act_gva, &act, sizeof(act)) < 0) {
            pthread_mutex_unlock(&sig_lock);
            return -LINUX_EFAULT;
        }
        sig_state.actions[idx] = act;
    }

    pthread_mutex_unlock(&sig_lock);
    return 0;
}

/* ---------- rt_sigprocmask ---------- */

int64_t signal_rt_sigprocmask(guest_t *g, int how,
                               uint64_t set_gva, uint64_t oldset_gva,
                               uint64_t sigsetsize) {
    if (sigsetsize != 8) return -LINUX_EINVAL;

    /* Return old mask if requested */
    if (oldset_gva) {
        if (guest_write(g, oldset_gva, &sig_state.blocked, 8) < 0)
            return -LINUX_EFAULT;
    }

    /* Apply new mask if provided */
    if (set_gva) {
        uint64_t set;
        if (guest_read(g, set_gva, &set, 8) < 0)
            return -LINUX_EFAULT;

        /* Never allow blocking SIGKILL or SIGSTOP */
        uint64_t unmaskable = sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP);

        switch (how) {
        case LINUX_SIG_BLOCK:
            sig_state.blocked |= set;
            break;
        case LINUX_SIG_UNBLOCK:
            sig_state.blocked &= ~set;
            break;
        case LINUX_SIG_SETMASK:
            sig_state.blocked = set;
            break;
        default:
            return -LINUX_EINVAL;
        }
        sig_state.blocked &= ~unmaskable;
    }

    return 0;
}

/* ---------- rt_sigsuspend ---------- */

int64_t signal_rt_sigsuspend(guest_t *g, uint64_t mask_gva,
                              uint64_t sigsetsize) {
    if (sigsetsize != 8) return -LINUX_EINVAL;

    if (mask_gva) {
        uint64_t mask;
        if (guest_read(g, mask_gva, &mask, 8) < 0)
            return -LINUX_EFAULT;

        /* Save original blocked mask for restoration after signal delivery */
        uint64_t saved_blocked = sig_state.blocked;

        /* Temporarily set blocked mask (never block SIGKILL/SIGSTOP) */
        uint64_t unmaskable = sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP);
        sig_state.blocked = mask & ~unmaskable;

        /* If no signal is pending with the new mask, restore immediately.
         * In a real kernel, sigsuspend blocks until a signal arrives. We
         * can't truly block (single-threaded vCPU model), so we just check
         * if any signal became deliverable. If yes, the vCPU loop will
         * deliver it. If no, restore the mask — the caller will loop. */
        if (!(sig_state.pending & ~sig_state.blocked)) {
            sig_state.blocked = saved_blocked;
        }
        /* If a signal IS pending, the mask stays temporarily modified.
         * signal_deliver() will execute the handler, and rt_sigreturn
         * will restore uc_sigmask. But we need to set uc_sigmask to the
         * ORIGINAL mask (saved_blocked), not the sigsuspend mask. Store
         * it for signal_deliver to use. */
        else {
            sig_state.saved_blocked = saved_blocked;
            sig_state.saved_blocked_valid = 1;
        }
    }

    /* Always return -EINTR. */
    return -LINUX_EINTR;
}

/* ---------- rt_sigpending ---------- */

int64_t signal_rt_sigpending(guest_t *g, uint64_t set_gva,
                              uint64_t sigsetsize) {
    if (sigsetsize != 8) return -LINUX_EINVAL;
    if (!set_gva) return -LINUX_EFAULT;

    /* Return signals that are pending AND blocked */
    uint64_t result = sig_state.pending & sig_state.blocked;
    if (guest_write(g, set_gva, &result, 8) < 0)
        return -LINUX_EFAULT;
    return 0;
}

/* ---------- Signal delivery ---------- */

/* FPSIMD context header (required by musl for setjmp/longjmp).
 * Linux places this immediately after sigcontext.__reserved starts. */
#define FPSIMD_MAGIC      0x46508001U
#define FPSIMD_CONTEXT_SIZE (8 + 4 + 4 + 32 * 16 + 4 + 4)  /* 528 bytes */

/* Build a minimal FPSIMD context block in the __reserved area. */
static void build_fpsimd_context(uint8_t *reserved) {
    /* struct fpsimd_context { __u32 head.magic, head.size;
     *   __u32 fpsr, fpcr; __uint128_t vregs[32]; } */
    uint32_t magic = FPSIMD_MAGIC;
    uint32_t size = FPSIMD_CONTEXT_SIZE;
    memcpy(reserved, &magic, 4);
    memcpy(reserved + 4, &size, 4);
    /* fpsr=0, fpcr=0, vregs[32]=0 — all zeroed (already zero from memset) */

    /* Terminator: zero magic/size after FPSIMD block */
    memset(reserved + FPSIMD_CONTEXT_SIZE, 0, 8);
}

int signal_deliver(hv_vcpu_t vcpu, guest_t *g, int *exit_code) {
    uint64_t deliverable = sig_state.pending & ~sig_state.blocked;
    if (deliverable == 0) return 0;

    /* Find lowest pending unblocked signal */
    int signum = __builtin_ctzll(deliverable) + 1;
    sig_state.pending &= ~sig_bit(signum);

    int idx = signum - 1;
    linux_sigaction_t *act = &sig_state.actions[idx];

    /* Check handler type */
    if (act->sa_handler == LINUX_SIG_IGN) {
        /* Ignored — discard signal */
        return 0;
    }

    if (act->sa_handler == LINUX_SIG_DFL) {
        /* Apply default disposition */
        sig_disposition_t disp = signal_default_disposition(signum);
        switch (disp) {
        case SIG_DISP_TERM:
        case SIG_DISP_CORE:
            *exit_code = 128 + signum;
            return -1;  /* Terminate */
        case SIG_DISP_IGN:
        case SIG_DISP_CONT:
        case SIG_DISP_STOP:
            return 0;   /* Ignore (STOP/CONT not meaningful for us) */
        }
    }

    /* Deliver to user handler: build rt_sigframe on guest stack */

    /* 1. Save current vCPU state */
    uint64_t saved_regs[31];
    uint64_t saved_sp, saved_pc, saved_pstate;

    for (int i = 0; i < 31; i++)
        hv_vcpu_get_reg(vcpu, HV_REG_X0 + i, &saved_regs[i]);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &saved_sp);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &saved_pc);
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, &saved_pstate);

    /* 2. Build the rt_sigframe */
    linux_rt_sigframe_t frame;
    memset(&frame, 0, sizeof(frame));

    /* siginfo */
    frame.info.si_signo = signum;
    frame.info.si_code = LINUX_SI_USER;
    frame.info.si_pid = (int32_t)proc_get_pid();

    /* ucontext */
    frame.uc.uc_flags = 0;
    frame.uc.uc_link = 0;

    /* If delivering from sigsuspend, store the ORIGINAL blocked mask so
     * rt_sigreturn restores it (not the temporary sigsuspend mask). */
    if (sig_state.saved_blocked_valid) {
        frame.uc.uc_sigmask = sig_state.saved_blocked;
        sig_state.saved_blocked_valid = 0;
    } else {
        frame.uc.uc_sigmask = sig_state.blocked;
    }

    /* sigcontext — save all registers */
    frame.uc.uc_mcontext.fault_address = 0;
    for (int i = 0; i < 31; i++)
        frame.uc.uc_mcontext.regs[i] = saved_regs[i];
    frame.uc.uc_mcontext.sp = saved_sp;
    frame.uc.uc_mcontext.pc = saved_pc;
    frame.uc.uc_mcontext.pstate = saved_pstate;

    /* FPSIMD context in __reserved area (musl reads this) */
    build_fpsimd_context(frame.uc.uc_mcontext.__reserved);

    /* 3. Push frame onto guest stack (16-byte aligned) */
    uint64_t frame_sp = (saved_sp - sizeof(frame)) & ~15ULL;

    if (guest_write(g, frame_sp, &frame, sizeof(frame)) < 0) {
        /* Can't write frame — terminate with default disposition */
        *exit_code = 128 + signum;
        return -1;
    }

    /* 4. Redirect vCPU to signal handler */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, frame_sp);
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, act->sa_handler);
    /* SPSR_EL1: EL0t (user mode) */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, 0);

    /* X0 = signal number */
    hv_vcpu_set_reg(vcpu, HV_REG_X0, (uint64_t)signum);

    /* X30 (LR) = sa_restorer (musl's __restore_rt which calls rt_sigreturn) */
    hv_vcpu_set_reg(vcpu, HV_REG_X30, act->sa_restorer);

    if (act->sa_flags & LINUX_SA_SIGINFO) {
        /* X1 = pointer to siginfo, X2 = pointer to ucontext */
        uint64_t siginfo_addr = frame_sp;
        uint64_t ucontext_addr = frame_sp + sizeof(linux_siginfo_t);
        hv_vcpu_set_reg(vcpu, HV_REG_X1, siginfo_addr);
        hv_vcpu_set_reg(vcpu, HV_REG_X2, ucontext_addr);
    }

    /* 5. Update blocked mask during handler execution */
    if (!(act->sa_flags & LINUX_SA_NODEFER))
        sig_state.blocked |= sig_bit(signum);
    sig_state.blocked |= act->sa_mask;
    /* Never block SIGKILL/SIGSTOP */
    sig_state.blocked &= ~(sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP));

    /* 6. Reset to SIG_DFL if SA_RESETHAND is set */
    if (act->sa_flags & LINUX_SA_RESETHAND) {
        act->sa_handler = LINUX_SIG_DFL;
        act->sa_flags &= ~LINUX_SA_SIGINFO;
    }

    return 1;
}

/* ---------- rt_sigreturn ---------- */

int signal_rt_sigreturn(hv_vcpu_t vcpu, guest_t *g) {
    /* Read SP_EL0 — frame was pushed at current SP */
    uint64_t sp;
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &sp);

    /* Read the rt_sigframe from guest stack */
    linux_rt_sigframe_t frame;
    if (guest_read(g, sp, &frame, sizeof(frame)) < 0) {
        fprintf(stderr, "hl: rt_sigreturn: failed to read frame at 0x%llx\n",
                (unsigned long long)sp);
        return -LINUX_EFAULT;
    }

    /* Restore all 31 GPRs */
    for (int i = 0; i < 31; i++)
        hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, frame.uc.uc_mcontext.regs[i]);

    /* Restore SP, PC, PSTATE */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, frame.uc.uc_mcontext.sp);
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, frame.uc.uc_mcontext.pc);
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, frame.uc.uc_mcontext.pstate);

    /* Restore signal mask */
    sig_state.blocked = frame.uc.uc_sigmask;
    sig_state.blocked &= ~(sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP));

    /* Return SYSCALL_EXEC_HAPPENED to skip the normal X0 writeback,
     * since we've restored the entire register set. */
    return SYSCALL_EXEC_HAPPENED;
}
