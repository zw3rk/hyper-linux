# Review follow-ups — items NOT fixed in the fix/review-findings pass

The adversarial review of `v0.2.4..master` produced ~60 findings. The branch
`fix/review-findings` fixes all critical and high-severity items plus a number
of mediums. This file records what was deliberately left, so nothing is lost.

Ordered by severity.

## SysV SHM (medium-high)

`src/syscall_shm.c` got its locking fixed (dispatch now holds `mmap_lock`) but
the following remain:

- **Segments are not shared across fork.** `segs[]`, `steal_cursor` and the
  `resolve_hooked` flag are process-local statics, and `fork_ipc.c` transfers
  no SysV state. The child inherits the guest PTEs via COW but has no Stage-2
  override, so a SHM VA silently resolves to its own private RAM. An X client
  that `shmat`s a MIT-SHM pixmap and then forks gets silent data divergence
  with no error. Fixing this needs SHM state in the IPC header plus a
  re-`hv_vm_map` in the child.
- **Slot leak on every `sys_shmat` error path.** `alloc_slot()` marks the slot
  used and the four error returns never clear it; 64 failures disable SysV SHM
  for the process.
- **Monotonic VA/IPA bump allocators are never reclaimed.** `steal_cursor` and
  `next_va` only advance, so ~1024 attach/detach cycles exhaust the 2GB bands
  even though the memory was released. A GTK client recreating its XShmImage
  on every resize hits this.
- **Detach does not invalidate the Stage-1 PTEs.** After `shmdt` the 2MB window
  stays mapped RW and aliases primary guest RAM once the IPA slice is restored,
  so a write-after-detach succeeds where Linux would SIGSEGV.
- **Guest-supplied `shmid` is adopted unfiltered.** `sys_shmat` will `shmctl`/
  `shmat` any host segment the guest names and map it into the guest address
  space, so a guest can enumerate and attach same-uid host segments.
- `shmctl` gaps: `IPC_INFO`/`SHM_INFO` return 0 without writing the buffer,
  `SHM_STAT` treats its argument as a shmid rather than an index, `IPC_SET`/
  `SHM_LOCK`/`SHM_UNLOCK` return EINVAL, and `IPC_RMID` discards the host
  result.

## Filesystem / VFS (medium)

- **`st_rdev` is double-encoded for virtual device nodes.** `hl_device_stat()`
  already returns a Linux `new_encode_dev` value and `translate_stat()` encodes
  it again, so `stat("/dev/dsp")` reports major 0 minor 1027 while
  `fstat()` on the same open fd reports major 4 minor 3 — they disagree and
  neither is (14,3). `dsp_stat` is also used for `/dev/mixer`.
- **`sys_statx` has no `/proc` or `/dev` interception** (`sys_newfstatat` has
  both). glibc >= 2.33 implements `stat()` via `statx`, so on a glibc guest
  `stat("/proc/self/exe")` fails while `open()` of the same path works.
- **Five ops route through the resolver only in rooted mode** (`fchmodat`,
  `fchownat`, `utimensat`, `renameat2`, `linkat`) while nine others resolve
  unconditionally. In legacy mode that reproduces the exact sysroot-redirect
  split v0.2.4 claimed to have fixed.
- **`HL_MOUNT_VIRTUAL` is only checked by `unlinkat`/`mkdirat`.** Other ops
  (statx, statfs, readlinkat, faccessat, xattr, truncate) execute against the
  real host `/dev` for a namespace that is meant to be synthetic.
- **`/proc/self/cwd` and `/proc/self/fd/N` return host paths.**
  `hl_vfs_host_to_guest()` exists for exactly this and is still only called
  from `app_open.c`, so musl's `realpath()` hands the guest `/Users/<name>/...`
  which then fails to reopen, leaking the host layout and username.
- `sys_chroot` still bypasses the resolver and stores a guest path as the host
  sysroot prefix.
- `getdents64` does not bound `name_len` against its 280-byte stack buffer.
  Unreachable on APFS (NAME_MAX 255) but the rooted profile can bind arbitrary
  filesystems. Pre-existing.

## Audio (medium)

- **No frame alignment anywhere in the transport.** A single guest write that
  is not a whole number of frames permanently shifts the frame phase for the
  rest of the stream (L/R swap / broadband garbage) until `SNDCTL_DSP_RESET`.
- **`SNDCTL_DSP_SYNC` still does not drain.** It no longer destroys queued
  audio (that was fixed), but the `drain` op is declared and never wired, so
  SYNC returns immediately instead of blocking until playback completes.
- **`GETODELAY` under-reports by the Audio Queue depth** (~93 ms): `completed`
  advances when PCM is handed to the queue, not when it is played.
- **poll readiness and write acceptance use different capacities.** Readiness
  comes from the socketpair's `SO_SNDBUF` (sized once at create) while
  acceptance uses `frag_size * frag_count`; after `SETFRAGMENT` they disagree,
  so a non-blocking OSS client can spin poll-ready/write-EAGAIN.
- `writev`/`readv`/`preadv`/`pwritev` still bypass `of->ops` (only `write`,
  `read`, `ioctl`, `fstat` dispatch), so vectored writes to `/dev/dsp` skip
  gain and the `accepted` accounting. The `ops->writev/readv/lseek/
  fcntl_getfl/fcntl_setfl` slots remain unwired.

## Networking (medium, pre-existing)

- **SCM_RIGHTS with more than 32 fds delivers raw host fd numbers to the
  guest and leaks them.** The translation branch is gated on
  `data_len <= sizeof(data_copy)` (128 bytes = 32 fds); larger arrays fall
  through untranslated. Byte-identical in v0.2.4, so not a regression.
- recvmsg control-buffer truncation `break`s without closing already-received
  SCM_RIGHTS fds and never sets `MSG_CTRUNC`.
- Abstract AF_UNIX sockets (`sun_path[0] == '\0'`) silently become empty-path
  filesystem sockets. Xlib's fallback masks it.

## Diagnostics / misc (low)

- `syscall_stats`' `SIGUSR1` handler is async-signal-unsafe (`pthread_mutex_lock`,
  `fprintf`, `qsort`, `getenv`) and can self-deadlock on `dump_lock`. Opt-in.
- `hl_trace_escape`/`hl_trace_path` are still dead code, so `HL_TRACE_REDACT`
  has no effect and guest-controlled path bytes reach the terminal unescaped
  under `--trace`.
- `app_open.c` remains entirely unreachable (no production caller). Audited
  clean — no `system`/`popen`, no injection surface — but it is dead weight.
- `tgkill` ignores `tgid` entirely and does not reject `tid <= 0`.
- `thread_find()`'s result is used after `thread_lock` is dropped (slot-reuse
  window).
- EL0 fault logging is unconditional (was verbose-only), which floods stderr
  for guests that use SIGSEGV as a control mechanism (GC/JIT guard pages).
- Several unchecked `malloc`/`strdup` in `hl.c` argv/env construction.
- Makefile `CFLAGS` still omit `-std=c11` and `-Werror=vla` that the project's
  own C guidelines mandate (pre-existing; the build is VLA-free in practice,
  machine-verified).

## Verification not performed

- The 4-mode `test/test-matrix.sh` (hl-x64, lima-aarch64, lima-x64) was not
  run — it needs Rosetta and a Lima VM. The page-table reuse change in
  `guest_map_va_range` touches the x86_64/rosetta path specifically and should
  get an `hl-x64` matrix run before release.
- No TSan run; the concurrency fixes are reasoned and reviewed, not
  dynamically verified.
