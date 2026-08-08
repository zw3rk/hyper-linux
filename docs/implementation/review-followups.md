# Review follow-ups — status

Two adversarial review rounds fed this branch. The first covered
`v0.2.4..master` (~60 findings). The second reviewed **this branch's own
commits**, because master had not moved — and found that several of the
first round's fixes had introduced new defects. This file is the closing
record for both.

## Round 1 — findings against master

All fixed. See the table in the git history of this file; every item is
covered by the commits up to `4f369ec..`, and the areas were: SysV SHM
sharing and allocator reclamation, `st_rdev` encoding, five path ops that
resolved only in rooted mode, mount-flag enforcement, `/proc` path leaks,
`getdents64` bounds, audio frame alignment and drain, SCM_RIGHTS fd
handling, abstract AF_UNIX sockets, `SIGUSR1` async-signal safety, trace
sanitisation, and build flags.

## Round 2 — regressions introduced by round 1

Each of these was a defect in the round-1 fix itself, and each is now fixed
with a regression test that **fails when that fix alone is reverted**
(mutation-tested, not merely written).

| Area | Defect | Test |
|---|---|---|
| Audio | RESET left `running` set: the stream could never restart | `test-audio-restart`, `test-audio-coreaudio` |
| SHM | second `shmat` orphaned the first window; a later segment reused it and silently corrupted it | `test-shm` |
| SHM | explicit `shmaddr` onto an occupied window returned success and aliased | `test-shm` |
| SHM | `shmget` in the parent + `shmat` in the child failed with EACCES | `test-shm` |
| FS | five `*at()` ops used `AT_FDCWD` in legacy mode → wrong directory, rc=0 | `test-at-dirfd` |
| FD | `F_DUPFD` ignored `arg` and cleared CLOEXEC | `test-fcntl-dup` |
| FD | stdio `status_flags` hard-coded 0; `F_SETFL` never reached the fd | `test-fcntl-dup` |
| FD | `dup3` over a live eventfd leaked its fd-keyed state | `test-fcntl-dup` |
| Poll | indefinite poll/select/epoll returned 0; wake bytes leaked | `test-poll-wakeup` |
| Net | abstract AF_UNIX dir was a fixed world-writable `/tmp` path | `test-abstract-unix` |
| Net | abstract names were not rebindable (EADDRINUSE) | `test-abstract-unix` |
| VFS | containment rejected legal ops on outward symlinks | `test-vfs-rootdir` |
| VFS | `opendir("/")` was ENOENT — `FD_VIRTUAL_DIR` was never implemented | `test-vfs-rootdir` |
| VFS | `fchdir` on a non-directory succeeded | `test-vfs-rootdir` |
| VFS | a bind whose root did not exist yet was dead with EACCES forever | `test-vfs-rootdir` |
| Proc | `/proc/self/exe` returned an unopenable host path | `test-vfs-rootdir` |
| FS | `O_CREAT\|O_EXCL` followed the final symlink | `test-fs-semantics` |
| FS | `l*xattr` nofollow was dead | `test-fs-semantics` |
| FS | no RO-mount guard on `setxattr`/`removexattr`/`utimensat` | `test-fs-semantics` |
| Dev | `fstat` on `/dev/null` reported the macOS device numbers | `test-dev-stat` |
| Diag | `HL_TRACE_REDACT` unreachable via `--trace=` | `test-diagnostics.sh` |
| Diag | crash report — the output meant for public issues — unredacted | `test-diagnostics.sh` |
| Diag | `SIGUSR1` dump lost when the guest issues no syscalls | `test-diagnostics.sh` |
| Diag | unchecked `strdup` truncated the copied environment | — |

### Also found, and pre-existing on master

- **`futex_interrupt_requested` was a sticky flag** set when the last worker
  thread exits and cleared nowhere but `execve`. From the first worker exit
  onward, every indefinite `futex_wait`/`poll`/`select`/`epoll_wait`
  returned EINTR immediately — and since correct callers retry on EINTR,
  that is a permanent 100%-CPU spin, not a visible hang. Replaced with a
  broadcast epoch. (`test-poll-wakeup`)

### Tests that asserted nothing

- `test-vfs-containment` printed SKIP and counted **nothing** when
  `symlink()` failed — which is exactly what was happening — and accepted
  any errno as containment. Now 10 real subtests against a target that
  provably exists outside the bind.
- `test-shm`'s reclamation check looped 40 times over a 1024-window band, so
  a fully leaking allocator could not fail it. Now 1100 cycles with an
  address-stability assertion.

## Remaining verification debt

1. **The 4-mode matrix has not been run on this branch.** Everything here
   was verified on `hl-aarch64`, in both fs modes, plus the host unit lane.
   Before release:
   ```
   nix develop -c bash test/test-matrix.sh all
   ```
   Pay particular attention to `hl-x64`: the `guest_map_va_range` reuse
   contract, the SHM Stage-2 changes and the poll/EINTR change all touch
   the rosetta path.

2. **No TSan run.** The concurrency work is reasoned and reviewed, not
   dynamically verified.

3. **`epoll_pwait`'s `poll_waiters` accounting is not test-covered.** The
   change makes the wake broadcast size match reality and matches what
   `poll` and `select` already do, but no guest-observable failure could be
   constructed for the deficit itself.

4. **SHM fork sharing is tested single-level.** Nested fork (grandchild) is
   still not exercised.

5. **`clone` cannot pass fds when a kqueue fd is open.** Observed while
   writing `test-poll-wakeup`: `clone: failed to send fds via SCM_RIGHTS` /
   `fork-child: fd count mismatch: received 0, expected 6`. The child ends
   up with no fds at all. Not investigated — filed here so it is not lost.

6. **"faccessat/fchmodat read undefined" did not reproduce.** The paths are
   assigned before use and clang's `-Wconditional-uninitialized` /
   `-Wsometimes-uninitialized` report nothing in `syscall_fs.c`. Recorded as
   not-reproduced rather than silently dropped.
