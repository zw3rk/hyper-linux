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
#define SYS_getcwd       17
#define SYS_dup          23
#define SYS_dup3         24
#define SYS_fcntl        25
#define SYS_ioctl        29
#define SYS_faccessat    48
#define SYS_chdir        49
#define SYS_openat       56
#define SYS_close        57
#define SYS_pipe2        59
#define SYS_getdents64   61
#define SYS_lseek        62
#define SYS_read         63
#define SYS_write        64
#define SYS_writev       66
#define SYS_readlinkat   78
#define SYS_newfstatat   79
#define SYS_fstat        80
#define SYS_exit         93
#define SYS_exit_group   94
#define SYS_set_tid_address 96
#define SYS_clock_gettime 113
#define SYS_rt_sigaction  134
#define SYS_rt_sigprocmask 135
#define SYS_uname        160
#define SYS_getpid       172
#define SYS_gettid       178
#define SYS_brk          214
#define SYS_munmap       215
#define SYS_mmap         222
#define SYS_mprotect     226
#define SYS_getrandom    278

/* ---------- Linux errno values ---------- */
#define LINUX_EPERM       1
#define LINUX_ENOENT      2
#define LINUX_ESRCH       3
#define LINUX_EINTR       4
#define LINUX_EIO         5
#define LINUX_EBADF       9
#define LINUX_ENOMEM     12
#define LINUX_EACCES     13
#define LINUX_EFAULT     14
#define LINUX_ENOTDIR    20
#define LINUX_EINVAL     22
#define LINUX_ENOTTY     25
#define LINUX_ENOSYS     38
#define LINUX_ERANGE     34

/* ---------- Linux ioctl constants ---------- */
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
#define LINUX_O_CLOEXEC  0x80000
#define LINUX_O_DIRECTORY 0x10000

/* ---------- Linux AT_* constants ---------- */
#define LINUX_AT_FDCWD   (-100)

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

/* ---------- Linux iovec ---------- */
typedef struct {
    uint64_t iov_base;  /* Guest pointer */
    uint64_t iov_len;
} linux_iovec_t;

/* ---------- FD table ---------- */
#define FD_TABLE_SIZE 256

#define FD_CLOSED   0
#define FD_STDIO    1
#define FD_REGULAR  2
#define FD_DIR      3

typedef struct {
    int type;     /* FD_CLOSED, FD_STDIO, FD_REGULAR, FD_DIR */
    int host_fd;  /* Underlying macOS file descriptor */
} fd_entry_t;

/* ---------- Auxiliary vector types ---------- */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_HWCAP   16
#define AT_CLKTCK  17
#define AT_RANDOM  25
#define AT_PLATFORM 45

/* ---------- API ---------- */

/* Initialize the syscall subsystem (FD table, etc.) */
void syscall_init(void);

/* Dispatch a syscall. Reads X8 (nr) and X0-X5 (args) from vCPU registers.
 * Writes result back to X0. Sets *exit_code if the process should exit.
 * Returns 0 to continue, 1 to exit. */
int syscall_dispatch(hv_vcpu_t vcpu, guest_t *g, int *exit_code, int verbose);

/* Build a Linux-compatible initial stack at the given stack_top.
 * Returns the initial SP (stack pointer) to pass to the guest. */
uint64_t build_linux_stack(guest_t *g, uint64_t stack_top,
                           int argc, const char **argv,
                           const elf_info_t *elf_info);

#endif /* SYSCALL_H */
