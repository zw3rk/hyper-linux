/* syscall.h — Linux aarch64 syscall dispatch for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include "guest.h"
#include "elf.h"

/* ---------- Linux aarch64 syscall numbers ---------- */
/* Sorted numerically for easy lookup.
 * Reference: include/uapi/asm-generic/unistd.h in Linux source. */
#define SYS_getcwd          17
#define SYS_epoll_create1   20
#define SYS_epoll_ctl       21
#define SYS_epoll_pwait     22
#define SYS_dup             23
#define SYS_dup3            24
#define SYS_fcntl           25
#define SYS_ioctl           29
#define SYS_inotify_init1   26
#define SYS_inotify_add_watch 27
#define SYS_inotify_rm_watch 28
#define SYS_flock           32
#define SYS_mknodat         33
#define SYS_mkdirat         34
#define SYS_unlinkat        35
#define SYS_symlinkat       36
#define SYS_linkat          37
#define SYS_truncate        45
#define SYS_statfs          43
#define SYS_fstatfs         44
#define SYS_ftruncate       46
#define SYS_fallocate       47
#define SYS_faccessat       48
#define SYS_chdir           49
#define SYS_fchdir          50
#define SYS_fchmod          52
#define SYS_fchmodat        53
#define SYS_fchownat        54
#define SYS_fchown          55
#define SYS_openat          56
#define SYS_close           57
#define SYS_pipe2           59
#define SYS_getdents64      61
#define SYS_lseek           62
#define SYS_read            63
#define SYS_write           64
#define SYS_readv           65
#define SYS_writev          66
#define SYS_pread64         67
#define SYS_pwrite64        68
#define SYS_splice          76
#define SYS_tee             77
#define SYS_vmsplice        75
#define SYS_sendfile        71
#define SYS_pselect6        72
#define SYS_ppoll           73
#define SYS_readlinkat      78
#define SYS_newfstatat      79
#define SYS_fstat           80
#define SYS_timerfd_create  85
#define SYS_timerfd_settime 86
#define SYS_timerfd_gettime 87
#define SYS_sync            81
#define SYS_fsync           82
#define SYS_fdatasync       83
#define SYS_utimensat       88
#define SYS_exit            93
#define SYS_exit_group      94
#define SYS_waitid          95
#define SYS_set_tid_address 96
#define SYS_futex           98
#define SYS_set_robust_list 99
#define SYS_getitimer       102
#define SYS_setitimer       103
#define SYS_nanosleep       101
#define SYS_clock_gettime   113
#define SYS_clock_nanosleep 115
#define SYS_sched_getaffinity 123
#define SYS_sched_yield     124
#define SYS_kill            129
#define SYS_tgkill          131
#define SYS_sigaltstack     132
#define SYS_rt_sigsuspend   133
#define SYS_rt_sigaction    134
#define SYS_rt_sigprocmask  135
#define SYS_rt_sigpending   136
#define SYS_rt_sigreturn    139
#define SYS_setpriority     140
#define SYS_getpriority     141
#define SYS_setregid        143
#define SYS_setgid          144
#define SYS_setreuid        145
#define SYS_setuid          146
#define SYS_setresuid       147
#define SYS_getresuid       148
#define SYS_setresgid       149
#define SYS_getresgid       150
#define SYS_setpgid         154
#define SYS_getpgid         155
#define SYS_setsid          157
#define SYS_getgroups       158
#define SYS_uname           160
#define SYS_getrusage       165
#define SYS_umask           166
#define SYS_prctl           167
#define SYS_gettimeofday    169
#define SYS_getpid          172
#define SYS_getppid         173
#define SYS_getuid          174
#define SYS_geteuid         175
#define SYS_getgid          176
#define SYS_getegid         177
#define SYS_gettid          178
#define SYS_sysinfo         179
#define SYS_socket          198
#define SYS_bind            200
#define SYS_listen          201
#define SYS_connect         203
#define SYS_accept          204
#define SYS_clone           220
#define SYS_execve          221
#define SYS_brk             214
#define SYS_munmap          215
#define SYS_mmap            222
#define SYS_mprotect        226
#define SYS_madvise         233
#define SYS_wait4           260
#define SYS_prlimit64       261
#define SYS_renameat2       276
#define SYS_getrandom       278
#define SYS_execveat        281
#define SYS_copy_file_range 285
#define SYS_statx           291
#define SYS_close_range     436

/* ---------- Linux errno values ---------- */
#define LINUX_EPERM       1
#define LINUX_ENOENT      2
#define LINUX_ESRCH       3
#define LINUX_EINTR       4
#define LINUX_EIO         5
#define LINUX_ENXIO       6
#define LINUX_E2BIG       7
#define LINUX_ENOEXEC     8
#define LINUX_EBADF       9
#define LINUX_EAGAIN     11   /* Also EWOULDBLOCK */
#define LINUX_ENOMEM     12
#define LINUX_EACCES     13
#define LINUX_EFAULT     14
#define LINUX_EBUSY      16
#define LINUX_EEXIST     17
#define LINUX_EXDEV      18
#define LINUX_ENODEV     19
#define LINUX_ENOTDIR    20
#define LINUX_EISDIR     21
#define LINUX_EINVAL     22
#define LINUX_ENFILE     23
#define LINUX_EMFILE     24
#define LINUX_ENOTTY     25
#define LINUX_ETXTBSY    26
#define LINUX_EFBIG      27
#define LINUX_ENOSPC     28
#define LINUX_ESPIPE     29
#define LINUX_EROFS      30
#define LINUX_EMLINK     31
#define LINUX_EPIPE      32
#define LINUX_ERANGE     34
#define LINUX_EDEADLK    35
#define LINUX_ENAMETOOLONG 36
#define LINUX_ENOLCK     37
#define LINUX_ENOSYS     38
#define LINUX_ENOTEMPTY  39
#define LINUX_ELOOP      40
#define LINUX_ENOPROTOOPT 92
#define LINUX_ECHILD     10
#define LINUX_EOPNOTSUPP 95
#define LINUX_EOVERFLOW  75

/* ---------- Linux FD flags ---------- */
#define LINUX_FD_CLOEXEC   1

/* ---------- Linux ioctl constants ---------- */
#define LINUX_TCGETS     0x5401
#define LINUX_TCSETS     0x5402
#define LINUX_TIOCGWINSZ 0x5413

/* ---------- Linux open flags ---------- */
#define LINUX_O_RDONLY   0x0000
#define LINUX_O_WRONLY   0x0001
#define LINUX_O_RDWR     0x0002
#define LINUX_O_CREAT    0x0040
#define LINUX_O_EXCL     0x0080
#define LINUX_O_TRUNC    0x0200
#define LINUX_O_APPEND   0x0400
#define LINUX_O_NONBLOCK 0x0800
/* aarch64-linux open flag values (from asm-generic/fcntl.h).
 * Note: these differ from x86_64-linux values! */
#define LINUX_O_DIRECTORY  0x4000   /* 040000 octal */
#define LINUX_O_NOFOLLOW   0x8000   /* 0100000 octal */
#define LINUX_O_DIRECT     0x10000  /* 0200000 octal */
#define LINUX_O_LARGEFILE  0x20000  /* 0400000 octal — ignored on LP64 */
#define LINUX_O_CLOEXEC    0x80000  /* 02000000 octal */

/* ---------- Linux AT_* constants ---------- */
#define LINUX_AT_FDCWD             (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW  0x100
#define LINUX_AT_REMOVEDIR         0x200
#define LINUX_AT_SYMLINK_FOLLOW    0x400
#define LINUX_AT_EMPTY_PATH        0x1000

/* ---------- Linux futex operations ---------- */
#define LINUX_FUTEX_WAIT  0
#define LINUX_FUTEX_WAKE  1

/* ---------- Linux prctl operations ---------- */
#define LINUX_PR_SET_NAME  15
#define LINUX_PR_GET_NAME  16

/* ---------- Linux mmap flags ---------- */
#define LINUX_PROT_NONE  0x0
#define LINUX_PROT_READ  0x1
#define LINUX_PROT_WRITE 0x2
#define LINUX_PROT_EXEC  0x4

#define LINUX_MAP_SHARED    0x01
#define LINUX_MAP_PRIVATE   0x02
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20

/* ---------- Linux struct stat (aarch64) ---------- */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    int64_t  st_atime_nsec;
    int64_t  st_mtime_sec;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime_sec;
    int64_t  st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
} linux_stat_t;

/* ---------- Linux struct utsname ---------- */
#define LINUX_UTSNAME_LEN 65

typedef struct {
    char sysname[LINUX_UTSNAME_LEN];
    char nodename[LINUX_UTSNAME_LEN];
    char release[LINUX_UTSNAME_LEN];
    char version[LINUX_UTSNAME_LEN];
    char machine[LINUX_UTSNAME_LEN];
    char domainname[LINUX_UTSNAME_LEN];
} linux_utsname_t;

/* ---------- Linux struct timespec ---------- */
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} linux_timespec_t;

/* ---------- Linux struct timeval (aarch64) ---------- */
typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} linux_timeval_t;

/* ---------- Linux struct statfs (aarch64) ---------- */
typedef struct {
    int64_t  f_type;
    int64_t  f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t  f_fsid[2];
    int64_t  f_namelen;
    int64_t  f_frsize;
    int64_t  f_flags;
    int64_t  f_spare[4];
} linux_statfs_t;

/* ---------- Linux iovec ---------- */
typedef struct {
    uint64_t iov_base;  /* Guest pointer */
    uint64_t iov_len;
} linux_iovec_t;

/* ---------- Linux struct sysinfo ---------- */
typedef struct {
    int64_t  uptime;
    uint64_t loads[3];       /* 1, 5, 15 minute load averages × 65536 */
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t pad2;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char     _f[4];          /* Padding to 64 bytes on LP64 */
} linux_sysinfo_t;

/* ---------- Linux struct rusage ---------- */
typedef struct {
    linux_timeval_t ru_utime;
    linux_timeval_t ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
} linux_rusage_t;

/* ---------- Linux struct rlimit64 ---------- */
typedef struct {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} linux_rlimit64_t;

/* ---------- Linux struct pollfd ---------- */
typedef struct {
    int32_t  fd;
    int16_t  events;
    int16_t  revents;
} linux_pollfd_t;

/* ---------- Linux struct statx (aarch64) ---------- */
typedef struct {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    /* struct statx_timestamp: tv_sec(8) + tv_nsec(4) + __reserved(4) */
    int64_t  stx_atime_sec;
    uint32_t stx_atime_nsec;
    uint32_t __atime_pad;
    int64_t  stx_btime_sec;
    uint32_t stx_btime_nsec;
    uint32_t __btime_pad;
    int64_t  stx_ctime_sec;
    uint32_t stx_ctime_nsec;
    uint32_t __ctime_pad;
    int64_t  stx_mtime_sec;
    uint32_t stx_mtime_nsec;
    uint32_t __mtime_pad;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t __spare2[13];
} linux_statx_t;

/* statx mask bits */
#define STATX_TYPE        0x0001U
#define STATX_MODE        0x0002U
#define STATX_NLINK       0x0004U
#define STATX_UID         0x0008U
#define STATX_GID         0x0010U
#define STATX_ATIME       0x0020U
#define STATX_MTIME       0x0040U
#define STATX_CTIME       0x0080U
#define STATX_INO         0x0100U
#define STATX_SIZE        0x0200U
#define STATX_BLOCKS      0x0400U
#define STATX_BASIC_STATS 0x07FFU
#define STATX_BTIME       0x0800U

/* ---------- FD table ---------- */
#define FD_TABLE_SIZE 256

#define FD_CLOSED   0
#define FD_STDIO    1
#define FD_REGULAR  2
#define FD_DIR      3
#define FD_PIPE     4

typedef struct {
    int   type;        /* FD_CLOSED, FD_STDIO, FD_REGULAR, FD_DIR */
    int   host_fd;     /* Underlying macOS file descriptor */
    int   linux_flags; /* Linux open flags (for CLOEXEC tracking) */
    void *dir;         /* DIR* for FD_DIR entries (NULL otherwise) */
} fd_entry_t;

/* ---------- API ---------- */

/* Initialize the syscall subsystem (FD table, etc.) */
void syscall_init(void);

/* Dispatch a syscall. Reads X8 (nr) and X0-X5 (args) from vCPU registers.
 * Writes result back to X0. Sets *exit_code if the process should exit.
 * Returns 0 to continue, 1 to exit. */
int syscall_dispatch(hv_vcpu_t vcpu, guest_t *g, int *exit_code, int verbose);

#endif /* SYSCALL_H */
