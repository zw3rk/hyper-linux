# Review follow-ups — status

The adversarial review of `v0.2.4..master` produced ~60 findings. Branch
`fix/review-findings` addresses all of them. This file is the closing record:
what was fixed, and what remains as verification debt rather than defects.

## Everything from the original follow-up list is now fixed

| Area | Item | Where |
|---|---|---|
| SHM | segments not shared across fork | `hl_shm_fork_export/import`, IPC v6 |
| SHM | slot leak on every shmat error path | `SHMAT_FAIL` in `sys_shmat` |
| SHM | monotonic VA/IPA allocators never reclaimed | first-fit scans over `segs[]` |
| SHM | detach left Stage-1 PTEs live | `guest_unmap_va_range()` |
| SHM | any guest-named shmid adopted and mapped | adopt only `shm_cpid == getpid()` |
| SHM | IPC_INFO/SHM_INFO/SHM_STAT/IPC_SET/SHM_LOCK/RMID gaps | `sys_shmctl` |
| FS | `st_rdev` double-encoded; stat vs fstat disagreed | `device.c`, `audio_oss.c` |
| FS | `statx` had no `/dev` or `/proc` interception | `sys_statx` |
| FS | five ops resolved only in rooted mode | `syscall_fs.c` |
| FS | `HL_MOUNT_VIRTUAL` only honoured by 2 of ~20 ops | mount-flag guards |
| FS | `/proc/self/cwd` and `/proc/self/fd/N` leaked host paths | `proc_emulation.c` |
| FS | `sys_chroot` bypassed the resolver | `sys_chroot` |
| FS | `getdents64` unbounded `name_len` | `sys_getdents64` |
| Audio | no frame alignment anywhere | CoreAudio carry buffer |
| Audio | `SNDCTL_DSP_SYNC` never drained | `hl_audio_stream_drain()` |
| Audio | `GETODELAY` short by the queue depth | per-buffer byte accounting |
| Audio | poll readiness vs write capacity mismatch | resize `SO_SNDBUF` in configure |
| Audio | `writev`/`readv` bypassed `of->ops` | `sys_writev` / `sys_readv` |
| Net | SCM_RIGHTS >32 fds passed raw host fds and leaked | larger buffer + close/CTRUNC |
| Net | control truncation leaked fds, no `MSG_CTRUNC` | `sys_recvmsg` |
| Net | abstract AF_UNIX sockets unusable | mapped to `/tmp/.hl-abstract` |
| Diag | `SIGUSR1` stats handler async-signal-unsafe | deferred to the syscall path |
| Diag | `HL_TRACE_REDACT` inert, escapes unsanitized | applied in `hl_tracev()` |
| Diag | `tgkill` ignored `tgid`, accepted `tid <= 0` | validated |
| Diag | `thread_find()` pointer escaped `thread_lock` | `thread_find_live_tid()` |
| Diag | EL0 fault log unconditional | rate-limited to 16 unless `--verbose` |
| Diag | unchecked `malloc`/`strdup` in `hl.c` | checked |
| Build | `CFLAGS` missing `-std=c11 -Werror=vla` | added; tree builds clean |
| Test | host unit suite never ran | `test-all` runs `test-host-units` |

`src/app_open.c` is intentionally not called in-tree: it is the host→guest
argv mapping used by the AppKit wrapper in a sibling repository (see
`docs/SIBLING-REPOS.md`). That is now documented in `app_open.h`, and its
host unit test runs as part of `make test-all`.

## Verification debt (not defects)

These are gaps in what was *checked*, not known bugs.

1. **The 4-mode matrix was not run.** `test/test-matrix.sh` needs Rosetta and
   a Lima VM. Everything here was verified on `hl-aarch64` only, in both fs
   modes. Before release, run:
   ```
   nix develop -c bash test/test-matrix.sh all
   ```
   Pay particular attention to `hl-x64`: the `guest_map_va_range` reuse
   contract and the SHM Stage-2 changes both touch the rosetta path.

2. **No TSan run.** The concurrency fixes (fd_lock/audio ordering, the CWD
   lock, `poll_waiters`, the deferred stats dump) are reasoned and reviewed,
   not dynamically verified.

3. **SHM fork sharing is tested single-level.** `test-shm` covers
   parent↔child; nested fork (grandchild) is not exercised.

4. **Audio changes are verified with the null backend.** Frame alignment,
   drain and the U8 silence value are correct by construction and covered by
   the OSS tests, but no test listens to real Core Audio output.
