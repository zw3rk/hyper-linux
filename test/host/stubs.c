/* Minimal stubs so host unit tests can link vfs/audio without full hl. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "syscall.h"

fd_entry_t fd_table[FD_TABLE_SIZE];
pthread_mutex_t fd_lock;

int fd_to_host(int guest_fd) {
    (void)guest_fd;
    return -1;
}

int64_t linux_errno(void) {
    return -errno;
}

/* hl_vdir_path lives in syscall_fs.c (not linked into host unit tests). */
const char *hl_vdir_path(const void *vdir) {
    (void)vdir;
    return NULL;
}
