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
#include "syscall_fd.h"     /* signalfd_notify */
#include "syscall_proc.h"   /* proc_get_pid, SYSCALL_EXEC_HAPPENED */
#include "thread.h"         /* current_thread, thread_entry_t */
#include "vdso.h"           /* VDSO_BASE */

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

/* ---------- Signal state (module-level, process-wide) ---------- */
static signal_state_t sig_state;

/* ---------- Per-thread pending fault info ----------
 * When a synchronous fault (BRK, segfault, etc.) needs to deliver a signal,
 * the caller sets this before signal_queue()+signal_deliver(). signal_deliver()
 * consumes it to populate si_code/si_addr/fault_address in the signal frame
 * instead of the default SI_USER/si_pid fields. Thread-local because each
 * vCPU thread delivers signals independently. */
typedef struct {
    int valid;          /* Non-zero if fault info is pending */
    int si_code;        /* e.g., LINUX_TRAP_BRKPT */
    uint64_t addr;      /* Fault address (BRK PC, segfault addr, etc.) */
} pending_fault_t;

static _Thread_local pending_fault_t pending_fault;

/* Protects signal actions array. Multiple threads may call rt_sigaction
 * concurrently (e.g., during musl init). Pending/blocked masks are
 * process-wide for the MVP; per-thread masks are deferred. */
static pthread_mutex_t sig_lock = PTHREAD_MUTEX_INITIALIZER; /* Lock order: 4 */

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

/* Per-thread signal mask accessors.  POSIX requires each thread to
 * have its own blocked mask.  Falls back to sig_state.blocked when
 * current_thread is NULL (early startup, before threads are initialized). */
static inline uint64_t *thread_blocked_ptr(void) {
    if (current_thread) return &current_thread->blocked;
    return &sig_state.blocked;
}
static inline uint64_t *thread_saved_blocked_ptr(void) {
    if (current_thread) return &current_thread->saved_blocked;
    return &sig_state.saved_blocked;
}
static inline int *thread_saved_valid_ptr(void) {
    if (current_thread) return &current_thread->saved_blocked_valid;
    return &sig_state.saved_blocked_valid;
}

/* ---------- Public API ---------- */

void signal_init(void) {
    memset(&sig_state, 0, sizeof(sig_state));
    sig_state.altstack.ss_flags = LINUX_SS_DISABLE;
}

void signal_reset_for_exec(void) {
    pthread_mutex_lock(&sig_lock);
    for (int i = 0; i < LINUX_NSIG; i++) {
        /* POSIX: handlers reset to SIG_DFL, except SIG_IGN stays SIG_IGN.
         * Pending signals and signal mask are preserved across exec. */
        if (sig_state.actions[i].sa_handler != LINUX_SIG_IGN) {
            sig_state.actions[i].sa_handler = LINUX_SIG_DFL;
            sig_state.actions[i].sa_flags = 0;
            sig_state.actions[i].sa_restorer = 0;
            sig_state.actions[i].sa_mask = 0;
        }
    }
    /* Clear any saved sigsuspend state (both global and per-thread) */
    sig_state.saved_blocked_valid = 0;
    if (current_thread)
        current_thread->saved_blocked_valid = 0;
    pthread_mutex_unlock(&sig_lock);
}

void signal_queue(int signum) {
    if (signum < 1 || signum > LINUX_NSIG) return;
    pthread_mutex_lock(&sig_lock);
    sig_state.pending |= sig_bit(signum);
    /* RT signals: increment queue count (multiple instances tracked) */
    if (signum >= LINUX_SIGRTMIN) {
        int idx = signum - LINUX_SIGRTMIN;
        if (sig_state.rt_queue[idx] < RT_SIGQUEUE_MAX)
            sig_state.rt_queue[idx]++;
    }
    pthread_mutex_unlock(&sig_lock);

    /* Notify any signalfd instances whose mask includes this signal.
     * This makes the signalfd pipe readable so poll/epoll sees it. */
    signalfd_notify(signum);

    /* Force all vCPUs out of hv_vcpu_run() so the signal can be
     * delivered promptly — even if the guest is in a tight loop
     * with no syscalls. Each vCPU's CANCELED handler will check
     * signal_pending() and call signal_deliver(). */
    thread_interrupt_all();
}

void signal_set_fault_info(int si_code, uint64_t addr) {
    pending_fault.valid = 1;
    pending_fault.si_code = si_code;
    pending_fault.addr = addr;
}

void signal_consume(int signum) {
    if (signum < 1 || signum > LINUX_NSIG) return;
    pthread_mutex_lock(&sig_lock);
    if (signum >= LINUX_SIGRTMIN) {
        /* RT signals: decrement queue; only clear pending bit at zero */
        int idx = signum - LINUX_SIGRTMIN;
        if (sig_state.rt_queue[idx] > 0)
            sig_state.rt_queue[idx]--;
        if (sig_state.rt_queue[idx] == 0)
            sig_state.pending &= ~sig_bit(signum);
    } else {
        sig_state.pending &= ~sig_bit(signum);
    }
    pthread_mutex_unlock(&sig_lock);
}

int signal_pending(void) {
    pthread_mutex_lock(&sig_lock);
    int result = (sig_state.pending & ~*thread_blocked_ptr()) != 0;
    pthread_mutex_unlock(&sig_lock);
    return result;
}

const signal_state_t *signal_get_state(void) {
    return &sig_state;
}

void signal_set_state(const signal_state_t *state) {
    if (state) sig_state = *state;
}

uint64_t signal_save_blocked(void) {
    pthread_mutex_lock(&sig_lock);
    uint64_t saved = *thread_blocked_ptr();
    pthread_mutex_unlock(&sig_lock);
    return saved;
}

void signal_set_blocked(uint64_t mask) {
    uint64_t unmaskable = sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP);
    pthread_mutex_lock(&sig_lock);
    *thread_blocked_ptr() = mask & ~unmaskable;
    pthread_mutex_unlock(&sig_lock);
}

void signal_restore_blocked(uint64_t saved) {
    uint64_t unmaskable = sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP);
    pthread_mutex_lock(&sig_lock);
    *thread_blocked_ptr() = saved & ~unmaskable;
    pthread_mutex_unlock(&sig_lock);
}

/* ---------- Guest ITIMER_REAL API ---------- */

/* Get monotonic time as timeval. Uses CLOCK_MONOTONIC to avoid NTP drift;
 * wall-clock adjustments must not affect timer expiry calculations. */
static struct timeval monotonic_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (struct timeval){ .tv_sec = ts.tv_sec, .tv_usec = ts.tv_nsec / 1000 };
}

/* Helper: compare timevals. Returns <0, 0, >0. */
static int timeval_cmp(const struct timeval *a, const struct timeval *b) {
    if (a->tv_sec != b->tv_sec) return (a->tv_sec < b->tv_sec) ? -1 : 1;
    if (a->tv_usec != b->tv_usec) return (a->tv_usec < b->tv_usec) ? -1 : 1;
    return 0;
}

/* Helper: add two timevals with overflow saturation. */
static struct timeval timeval_add(const struct timeval *a,
                                  const struct timeval *b) {
    struct timeval r = {
        .tv_sec  = a->tv_sec + b->tv_sec,
        .tv_usec = a->tv_usec + b->tv_usec,
    };
    /* Detect overflow: if both are positive and result is negative */
    if (a->tv_sec > 0 && b->tv_sec > 0 && r.tv_sec < 0) {
        r.tv_sec = __LONG_MAX__;
        r.tv_usec = 999999;
        return r;
    }
    if (r.tv_usec >= 1000000) {
        r.tv_sec += 1;
        r.tv_usec -= 1000000;
    }
    return r;
}

/* Helper: subtract b from a (a must be >= b).
 * Clamps to zero if a < b to avoid negative results. */
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
    /* Clamp underflow to zero */
    if (r.tv_sec < 0) {
        r.tv_sec = 0;
        r.tv_usec = 0;
    }
    return r;
}

void signal_set_itimer(const struct timeval *value,
                       const struct timeval *interval,
                       struct timeval *old_value,
                       struct timeval *old_interval) {
    struct timeval now = monotonic_now();

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
            struct timeval now = monotonic_now();
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

    struct timeval now = monotonic_now();

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
            fprintf(stderr, "hl: rt_sigaction: guest_read failed for "
                    "act_gva=0x%llx signum=%d\n",
                    (unsigned long long)act_gva, signum);
            pthread_mutex_unlock(&sig_lock);
            return -LINUX_EFAULT;
        }
        fprintf(stderr, "hl: rt_sigaction: signum=%d handler=0x%llx "
                "flags=0x%llx restorer=0x%llx mask=0x%llx\n",
                signum, (unsigned long long)act.sa_handler,
                (unsigned long long)act.sa_flags,
                (unsigned long long)act.sa_restorer,
                (unsigned long long)act.sa_mask);

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

    pthread_mutex_lock(&sig_lock);
    uint64_t *blocked = thread_blocked_ptr();

    /* Return old mask if requested */
    if (oldset_gva) {
        if (guest_write(g, oldset_gva, blocked, 8) < 0) {
            pthread_mutex_unlock(&sig_lock);
            return -LINUX_EFAULT;
        }
    }

    /* Apply new mask if provided */
    if (set_gva) {
        uint64_t set;
        if (guest_read(g, set_gva, &set, 8) < 0) {
            pthread_mutex_unlock(&sig_lock);
            return -LINUX_EFAULT;
        }

        /* Never allow blocking SIGKILL or SIGSTOP */
        uint64_t unmaskable = sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP);

        switch (how) {
        case LINUX_SIG_BLOCK:
            *blocked |= set;
            break;
        case LINUX_SIG_UNBLOCK:
            *blocked &= ~set;
            break;
        case LINUX_SIG_SETMASK:
            *blocked = set;
            break;
        default:
            pthread_mutex_unlock(&sig_lock);
            return -LINUX_EINVAL;
        }
        *blocked &= ~unmaskable;
    }

    pthread_mutex_unlock(&sig_lock);
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

        pthread_mutex_lock(&sig_lock);
        uint64_t *blocked = thread_blocked_ptr();
        uint64_t *saved_ptr = thread_saved_blocked_ptr();
        int *valid_ptr = thread_saved_valid_ptr();

        /* Save original blocked mask for restoration after signal delivery */
        uint64_t saved_blocked = *blocked;

        /* Temporarily set blocked mask (never block SIGKILL/SIGSTOP) */
        uint64_t unmaskable = sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP);
        *blocked = mask & ~unmaskable;

        /* If no signal is pending with the new mask, restore immediately.
         * In a real kernel, sigsuspend blocks until a signal arrives. We
         * can't truly block (single-threaded vCPU model), so we just check
         * if any signal became deliverable. If yes, the vCPU loop will
         * deliver it. If no, restore the mask — the caller will loop. */
        if (!(sig_state.pending & ~*blocked)) {
            *blocked = saved_blocked;
        }
        /* If a signal IS pending, the mask stays temporarily modified.
         * signal_deliver() will execute the handler, and rt_sigreturn
         * will restore uc_sigmask. But we need to set uc_sigmask to the
         * ORIGINAL mask (saved_blocked), not the sigsuspend mask. Store
         * it for signal_deliver to use. */
        else {
            *saved_ptr = saved_blocked;
            *valid_ptr = 1;
        }

        pthread_mutex_unlock(&sig_lock);
    }

    /* Always return -EINTR. */
    return -LINUX_EINTR;
}

/* ---------- rt_sigpending ---------- */

int64_t signal_rt_sigpending(guest_t *g, uint64_t set_gva,
                              uint64_t sigsetsize) {
    if (sigsetsize != 8) return -LINUX_EINVAL;
    if (!set_gva) return -LINUX_EFAULT;

    pthread_mutex_lock(&sig_lock);
    /* Return signals that are pending AND blocked by this thread */
    uint64_t result = sig_state.pending & *thread_blocked_ptr();
    pthread_mutex_unlock(&sig_lock);

    if (guest_write(g, set_gva, &result, 8) < 0)
        return -LINUX_EFAULT;
    return 0;
}

/* ---------- sigaltstack ---------- */

int64_t signal_sigaltstack(guest_t *g, uint64_t ss_gva, uint64_t old_ss_gva) {
    pthread_mutex_lock(&sig_lock);

    /* Return current altstack if requested */
    if (old_ss_gva) {
        linux_stack_t old_ss = sig_state.altstack;
        /* Set SS_ONSTACK if currently delivering a signal on the altstack */
        if (sig_state.on_altstack)
            old_ss.ss_flags |= LINUX_SS_ONSTACK;
        if (guest_write(g, old_ss_gva, &old_ss, sizeof(old_ss)) < 0) {
            pthread_mutex_unlock(&sig_lock);
            return -LINUX_EFAULT;
        }
    }

    /* Install new altstack if provided */
    if (ss_gva) {
        /* Cannot change altstack while executing on it */
        if (sig_state.on_altstack) {
            pthread_mutex_unlock(&sig_lock);
            return -LINUX_EPERM;
        }

        linux_stack_t ss;
        if (guest_read(g, ss_gva, &ss, sizeof(ss)) < 0) {
            pthread_mutex_unlock(&sig_lock);
            return -LINUX_EFAULT;
        }

        if (ss.ss_flags & LINUX_SS_DISABLE) {
            /* Disable the altstack */
            sig_state.altstack.ss_sp = 0;
            sig_state.altstack.ss_flags = LINUX_SS_DISABLE;
            sig_state.altstack.ss_size = 0;
        } else {
            if (ss.ss_size < LINUX_MINSIGSTKSZ) {
                pthread_mutex_unlock(&sig_lock);
                return -LINUX_ENOMEM;
            }
            sig_state.altstack = ss;
            sig_state.altstack.ss_flags = 0;
        }
    }

    pthread_mutex_unlock(&sig_lock);
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
    pthread_mutex_lock(&sig_lock);
    uint64_t *blocked = thread_blocked_ptr();
    uint64_t *saved_ptr = thread_saved_blocked_ptr();
    int *valid_ptr = thread_saved_valid_ptr();
    uint64_t deliverable = sig_state.pending & ~*blocked;
    if (deliverable == 0) {
        pthread_mutex_unlock(&sig_lock);
        return 0;
    }

    /* Find lowest pending unblocked signal */
    int signum = __builtin_ctzll(deliverable) + 1;

    /* Dequeue: for RT signals, decrement count and only clear the
     * pending bit when the queue is empty. Standard signals are
     * always cleared (single instance, bitmask semantics). */
    if (signum >= LINUX_SIGRTMIN) {
        int rt_idx = signum - LINUX_SIGRTMIN;
        if (sig_state.rt_queue[rt_idx] > 0)
            sig_state.rt_queue[rt_idx]--;
        if (sig_state.rt_queue[rt_idx] == 0)
            sig_state.pending &= ~sig_bit(signum);
    } else {
        sig_state.pending &= ~sig_bit(signum);
    }

    int idx = signum - 1;
    linux_sigaction_t *act = &sig_state.actions[idx];

    /* Check handler type */
    fprintf(stderr, "hl: signal_deliver: signum=%d handler=0x%llx "
            "flags=0x%llx\n", signum,
            (unsigned long long)act->sa_handler,
            (unsigned long long)act->sa_flags);
    if (act->sa_handler == LINUX_SIG_IGN) {
        /* Ignored — discard signal */
        pthread_mutex_unlock(&sig_lock);
        return 0;
    }

    if (act->sa_handler == LINUX_SIG_DFL) {
        /* Apply default disposition */
        sig_disposition_t disp = signal_default_disposition(signum);
        fprintf(stderr, "hl: signal_deliver: signum=%d handler=SIG_DFL "
                "disp=%d\n", signum, (int)disp);
        switch (disp) {
        case SIG_DISP_TERM:
        case SIG_DISP_CORE:
            *exit_code = 128 + signum;
            pthread_mutex_unlock(&sig_lock);
            return -1;  /* Terminate */
        case SIG_DISP_IGN:
        case SIG_DISP_CONT:
        case SIG_DISP_STOP:
            pthread_mutex_unlock(&sig_lock);
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

    /* siginfo — fault signals use si_code/si_addr from pending_fault;
     * non-fault signals use SI_USER with si_pid. */
    frame.info.si_signo = signum;
    if (pending_fault.valid) {
        frame.info.si_code = pending_fault.si_code;
        /* si_addr overlaps si_pid/si_uid at offset 16 in the siginfo union.
         * On aarch64-linux, si_addr is a 64-bit pointer occupying both
         * int32_t fields. Write it via memcpy to avoid strict aliasing. */
        memcpy(&frame.info.si_pid, &pending_fault.addr, 8);
    } else {
        frame.info.si_code = LINUX_SI_USER;
        frame.info.si_pid = (int32_t)proc_get_pid();
    }

    /* ucontext */
    frame.uc.uc_flags = 0;
    frame.uc.uc_link = 0;

    /* If delivering from sigsuspend, store the ORIGINAL blocked mask so
     * rt_sigreturn restores it (not the temporary sigsuspend mask). */
    if (*valid_ptr) {
        frame.uc.uc_sigmask = *saved_ptr;
        *valid_ptr = 0;
    } else {
        frame.uc.uc_sigmask = *blocked;
    }

    /* sigcontext — save all registers.
     * fault_address: for synchronous faults (BRK, segfault), set to the
     * faulting address; for asynchronous signals, zero. */
    frame.uc.uc_mcontext.fault_address =
        pending_fault.valid ? pending_fault.addr : 0;

    /* Consume the pending fault info (one-shot) */
    pending_fault.valid = 0;
    for (int i = 0; i < 31; i++)
        frame.uc.uc_mcontext.regs[i] = saved_regs[i];
    frame.uc.uc_mcontext.sp = saved_sp;
    frame.uc.uc_mcontext.pc = saved_pc;
    frame.uc.uc_mcontext.pstate = saved_pstate;

    /* FPSIMD context in __reserved area (musl reads this) */
    build_fpsimd_context(frame.uc.uc_mcontext.__reserved);

    /* Save the altstack info in uc_stack so gdb/tools can see it */
    frame.uc.uc_stack = sig_state.altstack;
    if (sig_state.on_altstack)
        frame.uc.uc_stack.ss_flags |= LINUX_SS_ONSTACK;

    /* 3. Determine stack for signal frame: use altstack if SA_ONSTACK
     * is set, an altstack is configured, and we're not already on it. */
    uint64_t signal_sp = saved_sp;
    int use_altstack = 0;
    if ((act->sa_flags & LINUX_SA_ONSTACK) &&
        !(sig_state.altstack.ss_flags & LINUX_SS_DISABLE) &&
        !sig_state.on_altstack) {
        /* Place frame at top of altstack (stack grows down) */
        signal_sp = sig_state.altstack.ss_sp + sig_state.altstack.ss_size;
        use_altstack = 1;
    }
    uint64_t frame_sp = (signal_sp - sizeof(frame)) & ~15ULL;

    if (guest_write(g, frame_sp, &frame, sizeof(frame)) < 0) {
        /* Can't write frame — terminate with default disposition */
        fprintf(stderr, "hl: signal_deliver: guest_write failed for "
                "frame_sp=0x%llx (signal_sp=0x%llx signum=%d "
                "frame_size=%zu)\n",
                (unsigned long long)frame_sp,
                (unsigned long long)signal_sp,
                signum, sizeof(frame));
        *exit_code = 128 + signum;
        pthread_mutex_unlock(&sig_lock);
        return -1;
    }

    /* 4. Redirect vCPU to signal handler */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, frame_sp);
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, act->sa_handler);
    /* SPSR_EL1: EL0t (user mode) */
    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, 0);

    /* X0 = signal number */
    hv_vcpu_set_reg(vcpu, HV_REG_X0, (uint64_t)signum);

    /* X30 (LR) = return address for signal handler.
     * On aarch64-linux, the kernel always sets LR to the vDSO's
     * __kernel_rt_sigreturn (mov x8,#139; svc #0; ret). The sa_restorer
     * field is architecturally unused on aarch64 — the kernel ignores it.
     * However, musl explicitly sets sa_restorer to its own __restore_rt
     * trampoline. We honor sa_restorer if set (for musl compatibility),
     * otherwise use the vDSO trampoline (for rosetta and any signal handler
     * that relies on kernel behavior). */
    uint64_t restorer = act->sa_restorer;
    if (restorer == 0) {
        /* vDSO __kernel_rt_sigreturn: VDSO_BASE + .text offset 0 */
        restorer = VDSO_BASE + 0xB0;
    }
    hv_vcpu_set_reg(vcpu, HV_REG_X30, restorer);

    if (act->sa_flags & LINUX_SA_SIGINFO) {
        /* X1 = pointer to siginfo, X2 = pointer to ucontext */
        uint64_t siginfo_addr = frame_sp;
        uint64_t ucontext_addr = frame_sp + sizeof(linux_siginfo_t);
        hv_vcpu_set_reg(vcpu, HV_REG_X1, siginfo_addr);
        hv_vcpu_set_reg(vcpu, HV_REG_X2, ucontext_addr);

    }

    /* 5. Update blocked mask during handler execution */
    if (!(act->sa_flags & LINUX_SA_NODEFER))
        *blocked |= sig_bit(signum);
    *blocked |= act->sa_mask;
    /* Never block SIGKILL/SIGSTOP */
    *blocked &= ~(sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP));

    /* 6. Track altstack usage */
    if (use_altstack)
        sig_state.on_altstack = 1;

    /* 7. Reset to SIG_DFL if SA_RESETHAND is set */
    if (act->sa_flags & LINUX_SA_RESETHAND) {
        act->sa_handler = LINUX_SIG_DFL;
        act->sa_flags &= ~LINUX_SA_SIGINFO;
    }

    pthread_mutex_unlock(&sig_lock);
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

    /* Restore signal mask and clear altstack-in-use flag */
    pthread_mutex_lock(&sig_lock);
    uint64_t *blocked = thread_blocked_ptr();
    *blocked = frame.uc.uc_sigmask;
    *blocked &= ~(sig_bit(LINUX_SIGKILL) | sig_bit(LINUX_SIGSTOP));
    sig_state.on_altstack = 0;
    pthread_mutex_unlock(&sig_lock);

    /* Return SYSCALL_EXEC_HAPPENED to skip the normal X0 writeback,
     * since we've restored the entire register set. */
    return SYSCALL_EXEC_HAPPENED;
}
