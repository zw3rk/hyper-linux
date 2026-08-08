# Remediation plan — round-3 review findings

All fixes land on `fix/review-findings`. Each gets a regression test that
FAILS when the fix alone is reverted (mutation-verified), and the full
`test-both-modes` suite must stay green. Order is by severity; HIGH first.

Key lesson driving this plan: the round-2 fixes passed their mutation tests
but introduced NEW defects in ADJACENT behaviour the tests never exercised.
So every fix here adds tests for the adjacent behaviour too, not just the
one line.

---

## H1 — `shmat` with explicit address must not unmap the guest's own memory
`src/syscall_shm.c` (sys_shmat, the explicit-`shmaddr` branch + the
`va_skipped` failure branch).

Root cause: `va_range_busy()` only knows SHM windows, so an explicit
`shmaddr` onto a non-SHM mapping passes the guard; then `guest_map_va_range_ex`
reports `va_skipped` and the cleanup `guest_unmap_va_range(guest_va,
guest_va+ipa_span)` tears down blocks it never installed — the guest's own
mapping.

Fix:
1. Before mapping, reject an explicit `shmaddr` that overlaps ANY live
   mapping, not just an SHM window. Walk `[guest_va, guest_va+ipa_span)` in
   2MB steps and if `guest_region_find(g, va)` returns non-NULL for any block,
   return `-EINVAL` **without unmapping anything** (Linux returns EINVAL for
   a busy address without SHM_REMAP). Keep the existing SHM `va_range_busy`
   check too (a stale SHM window may not be in the region table).
2. On the `va_skipped` failure branch, only unmap the blocks THIS call
   installed. Simplest correct form: since step 1 guarantees the range was
   free, `va_skipped` should now be 0; if it is ever non-zero, that is an
   internal invariant break — log and unmap only, but this path becomes
   unreachable for explicit addrs. Keep the unmap bounded to the span we own.

Tests (`test/test-shm.c`, guest): (−) `mmap` an anon region, `shmat` at its
2MB-aligned base → must be EINVAL AND the original mapping must still read
back its bytes (no SIGSEGV). (+) `shmat` at a genuinely free explicit 2MB
address still succeeds. Mutation: reverting the region-overlap check
reproduces the SIGSEGV.

---

## H2 — one `mprotect` on an attached SHM window must not permanently disable SHM
`src/guest.c` (guest_unmap_va_range) + `src/syscall_shm.c` (sys_shmdt).

Root cause: mprotect splits the window's L2 block into an L3 table;
`guest_unmap_va_range` deliberately leaves L3 table descriptors intact, so
the L2 slot still reads as valid ("mapped") and `hl_shm_find_guest_va`'s
first-fit returns the same dead VA forever.

Fix: a SHM window is always a full, hl-owned 2MB-aligned span, so on detach
it is safe to collapse a split block. Add a `collapse` variant (or a flag)
to `guest_unmap_va_range` used by `sys_shmdt`: when the L2 entry is a TABLE
descriptor, invalidate every L3 entry it points to and then clear the L2
entry to 0 (the L3 page stays in the bump pool, unreclaimed — acceptable).
Non-SHM callers keep the current "leave L3 tables intact" behaviour.

Tests (`test/test-shm.c`): (−) attach, `mprotect(RO)` the window, `shmdt`,
then `shmat` the SAME id AND a DIFFERENT id — both must succeed (base: both
EINVAL forever). Also assert the re-attach reads back freshly. Mutation:
skipping the L3 collapse reproduces the permanent EINVAL.

Note this also fixes SHM-F3 (MEDIUM): after collapse, a post-detach access
to the window faults instead of aliasing primary RAM. Add a (−) subtest that
a detached split window is unreadable.

---

## H3 — opening a `/dev` node `O_CLOEXEC` then `execve` must not abort hl
`src/syscall_exec.c` (the CLOEXEC sweep) + `src/syscall_fs.c` (close_range,
= MEDIUM V16).

Root cause: the exec sweep frees `.dir` with a blind `else free()`; a
FD_DEVICE fd stores a pointer into the static `g_nodes[]` there → free of
static storage → SIGABRT.

Fix: replace the blind `else free()` with the same whitelist `sys_close`
uses — `FD_DIR`→closedir, `FD_EPOLL`/`FD_VIRTUAL_DIR`→free, everything else
(incl. FD_DEVICE) → nothing. Apply the identical whitelist to `close_range`
(which currently frees only FD_EPOLL, leaking FD_VIRTUAL_DIR — H3's sibling
V16).

Tests: (−) guest opens `/dev/null` `O_CLOEXEC` then `execve`s a child — must
exit 0, not 134 (new `test-dev-cloexec` guest test; assert the child runs).
(−) `close_range` over many `open("/",O_DIRECTORY)` fds must not grow RSS
(guard: a bounded loop that would OOM if leaking — or assert via a
host-unit if RSS is awkward in-guest). Mutation: restoring the blind free
aborts; restoring FD_EPOLL-only in close_range leaks.

---

## H4 — poll/select must not turn already-ready results into EINTR
`src/syscall_poll.c` (ppoll `pure_wake`, pselect `pure_wake`, epoll).

Root cause: `if (--ret == 0) { pure_wake = 1; break; }` fires even when
`force_immediate`/`always_count` is set (a closed fd = POLLNVAL, or an
always-ready device like `/dev/mixer`), so the POLLNVAL/ready writeback is
skipped and the guest gets EINTR.

Fix: always DRAIN the wake byte and decrement `ret` for the invisible wake
fd, but set `pure_wake` (→ return EINTR) ONLY when there is nothing
guest-visible to report — i.e. `!force_immediate` (ppoll) / `always_count ==
0` (pselect) AND `ret == 0` after the drain. When force_immediate, fall
through to the normal writeback so POLLNVAL/ready reach the guest.

Tests (`test/test-poll-wakeup.c` + a new focused test): (−) with a wake byte
planted (fork+reap), poll a closed fd → POLLNVAL, never EINTR; poll
`/dev/mixer` → ready, never EINTR. (+) an indefinite poll with only the wake
still returns EINTR (the round-2 behaviour we keep). Mutation: dropping the
force_immediate guard reproduces the EINTR.

Also fix V12 (test hole): the epoll subtest of `test-poll-wakeup` passes
vacuously because fork fails with an epoll fd open. Re-trigger the wake
without fork (last-worker-exit path, as the poll agent did) so the epoll
assertion actually exercises a wake.

---

## H5 — the bind-containment check must be TOCTOU-safe
`src/vfs.c` (path resolution) + `src/syscall_fs.c` (path-op host calls).

Root cause: `path_within_mount` canonicalizes a path STRING and the host
syscall re-resolves that string, so a guest thread that rename-swaps a
component between check and use escapes the bind (verified: 6 escapes/8s at
HEAD, 0 at the strict rule). Reverting is not an option — the strict rule
refused legal outward-symlink ops.

Fix (pin the parent inode; macOS has `O_NOFOLLOW_ANY` since 10.15, confirmed
available): route mutating path ops through a helper that
  (a) resolves guest→host as today (interior symlinks resolved in guest
      namespace, containment-checked),
  (b) opens the resolved PARENT directory with
      `O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC` — this fails if ANY
      component became a symlink after resolution (the race), and pins the
      parent inode so a concurrent `rename` cannot move it,
  (c) re-verifies via `fcntl(F_GETPATH)` that the opened dir is still within
      the mount root,
  (d) performs the operation with the `*at(parent_fd, leaf, ...)` form and
      `AT_SYMLINK_NOFOLLOW` where the op is non-following.
For open()/openat of a regular file, open the final path with
`O_NOFOLLOW_ANY` added (hl has already resolved intended symlinks in guest
namespace, so a symlink appearing now IS the race). Follow ops on a final
symlink resolve in guest namespace and re-enter, re-pinning each time.

This is the largest change; implement it behind the existing resolver so
legacy mode and non-racing behaviour are unchanged. If full coverage proves
too invasive in one pass, phase it: phase 1 covers open/openat + the
mutating `*at` ops (renameat2/linkat/unlinkat/mkdirat/fchmodat/fchownat/
utimensat/symlinkat), which is the demonstrated escape surface; note any
op deferred to phase 2.

Tests (`test/test-vfs-containment.c`): (−) the two-thread rename-swap escape
must produce 0 reads of the outside secret over a fixed budget (base: >0);
(+) an ordinary in-bind symlink still resolves and legal outward-symlink
unlink/rename/readlink still work (the round-2 behaviour). Mutation: reverting
the pin reproduces the escape. Because it is a race, run the (−) case with a
generous try budget and assert exactly zero escapes.

---

## H6 — abstract-socket bind must not steal a live name from another process
`src/syscall_net.c` (sys_bind, the was_abstract branch).

Root cause: the unconditional `unlink()` deletes a live stand-in so a second
binder silently takes the name; Linux requires EADDRINUSE for a live name.

Fix: replace "lstat says socket → unlink" with the standard stale-socket
protocol. Try `bind()` first; on EADDRINUSE, probe liveness by `connect()`ing
a throwaway socket to the path: if connect SUCCEEDS, a live owner exists →
return EADDRINUSE (correct). If connect fails with ECONNREFUSED/ENOENT, the
socket is stale → `unlink()` and retry `bind()` once. Only ever remove a
socket with no live listener.

Tests (`test/test-abstract-unix.c` + a 2-process harness in the Makefile or
`test-diagnostics.sh`): (−) two concurrent hl, same name → the second bind
must fail EADDRINUSE (base: silently succeeds). (+) a STALE socket left by a
prior run is reclaimable — bind succeeds after the owner is gone (the
round-2 behaviour we keep). Mutation: restoring the unconditional unlink lets
the second bind succeed.

Also address the abstract-name regressions from the same commit:
- V25 (length 84→37): keep the mapped path short. Option: hash the abstract
  name into a fixed-width filename under the private dir, or use a shorter
  private dir. Pick the shortest scheme that keeps names ≤ Linux's 107 and
  fixes the collisions in V23.
- V23 (aliasing): the `'/'→'_'` mangling is not injective and NUL truncates.
  Encode the full counted byte string injectively (e.g. hex or a hash of the
  exact bytes incl. length), so distinct Linux names never collide.
- V24 (no self-heal): re-create the private dir on demand (restore a cheap
  existence check before use) so a tmp reaper cannot permanently break it.

---

## H7 — the vCPU watchdog must not kill a guest that is making progress
`src/syscall_proc.c` (vcpu_run_loop timeout handling) + `src/hl.c`
(timeout parsing + fork-child argv) + `hl.1` (docs).

Root cause: `alarm(timeout_sec)` per `hv_vcpu_run` iteration kills any guest
that runs 10s without a VM exit — i.e. any compute or vDSO-time-polling loop.
The vDSO (0.3.0-rc) made this common. No way to disable, not inherited by
fork children, and `hl.1` falsely claims it does not limit total execution.

Fix (distinguish "busy" from "wedged" by PC progress):
1. On alarm timeout, sample the guest PC. Compare to the PC saved at the
   previous timeout. If it ADVANCED (or SP/registers changed), the guest is
   making progress → reset a stuck-counter, re-arm, and continue. Only when
   the PC is identical for K consecutive windows (a true livelock/hang) do we
   report and exit. This keeps the safety net against genuine hangs while
   never killing a computing guest.
2. `--timeout 0` explicitly DISABLES the watchdog (`atoi` currently forces 0
   back to 10). Document 0 = unlimited.
3. Pass `--timeout <n>` into the `--fork-child` argv so children inherit it.
4. Fix `hl.1`: it IS a progress watchdog, describe the new semantics.

Tests (`test-diagnostics.sh` / a guest test): (−) a compute loop and a
`gettimeofday` polling loop each run >12s and COMPLETE (base: killed at 10s).
(−) a genuinely wedged guest (tight `b .` infinite self-branch, or a hang) is
still killed within a bounded time. (+) `--timeout 0` disables; a fork child
inherits the parent's timeout. Mutation: reverting the PC-progress check
kills the compute loop again.

---

## MEDIUM cluster (regressions I introduced; fix with the HIGH they sit next to)

- **V6 / fs#5 — device fstat lost across dup/dup2/F_DUPFD/fork.** Copy `.dir`
  in `hl_fd_dup_from` and `hl_fd_dup3`, and rebuild it for FD_DEVICE in the
  fork-IPC child (carry the node identity, e.g. by name/major:minor, in the
  IPC record). Test: fstat of a dup'd/forked `/dev/null` still reports 1:3.
- **V15 / fs#4 — newfstatat(AT_EMPTY_PATH) + OSS statx still host numbers.**
  Apply the `hl_device_fd_stat` override in `sys_newfstatat`'s AT_EMPTY_PATH
  branch, and route statx AT_EMPTY_PATH through `of->ops->fstat` for OSS
  nodes. Test: fstatat/statx AT_EMPTY on `/dev/null` and `/dev/dsp` agree
  with stat.
- **V13 / fs#2 — legacy `*at()` rejects a bad dirfd on an ABSOLUTE path.**
  `host_dirfd_for_op` should not validate the dirfd when the resolved path is
  absolute (Linux ignores dirfd then). Test: fchmodat/etc. with a closed
  dirfd + absolute path succeeds in legacy mode.
- **V14 / fs#3 — legacy renameat2 RENAME_NOREPLACE/EXCHANGE via a real
  dirfd.** Restore the flag handling for the dirfd case (use `renameatx_np`
  with the dirfds, or resolve to absolute then AT_FDCWD); check the unlinkat
  in the NOREPLACE fallback. Test: RENAME_EXCHANGE/NOREPLACE via a dirfd in
  legacy mode.
- **V17 / fs#7 + VFS-F7 — synthetic dir fd dead-end + fchdir ENOTDIR.** Make
  FD_VIRTUAL_DIR usable as a dirfd (openat/fstatat resolve its guest path),
  allow fchdir onto it (it IS a directory — set CWD to its guest path), and
  fork-inherit it. Test: `find /`, `fchdir(open("/"))`, and a fork child
  reading `/` all work.
- **V7 — SYNC drain 6s→75s.** Bound drain by WALL-CLOCK, not iteration count
  (the fix that made this regress). Cap total wait at ~5–6s as before. Test
  (host-unit): a pinned-full stream's drain returns within the cap.
- **V27 — environ OOM path wipes the environment.** The "keep original on
  strdup failure" is then destroyed by the argv memset. Fix so a failed copy
  leaves a usable environment (allocate the copy fully before repointing, and
  don't memset through env strings the guest still needs). Test (host or
  guest): induce the failure path, assert env survives.
- **V26 — crash-report redaction prefix-only.** Route the crash report
  through the same every-occurrence redaction `hl_tracev` uses, not the
  prefix-only `hl_trace_path`. Test (`test-diagnostics.sh`): a guest cmdline
  with `$HOME` mid-token is redacted in the crash report.

## MEDIUM (master-origin; propose but scope carefully)

- **V2 — [vvar]/[vdso]/page-table pool EL0-writable.** Map the pt-pool and
  vvar with EL0-no-access / EL0-RO AP bits in the low-block split. This
  touches `guest_split_block`/`guest_build_page_tables` permission encoding
  and risks destabilising the shim's own EL1 page-table writes; do it last,
  behind the full suite, and back out if it regresses. At minimum make
  `[vvar]` EL0-RO (its documented contract) even if the pt-pool proves
  harder. Test: EL0 write to `[vvar]` faults; the vDSO clock still works.
- **VFS-F2 — inotify_add_watch and bind(AF_UNIX) bypass the resolver.**
  Route both through `resolve_path_for_op_ex` like the other path ops. Test:
  in rooted mode, `inotify_add_watch("/etc")` and `bind("/tmp/x")` resolve
  through the mount table (fail/contained), not the raw host path.

## LOW (batch at the end, lower priority)
V8 (F_DUPFD legacy errno), V9 (drain -1 discarded), V18 (synthetic dir
dev/ino identity), V19 (EBADF vs ENOTDIR), VFS canonicalize_partial dstsz +
single-component + 32-truncation + nested-under-real-mount, V28 (stats
interleave), NET stale-socket accumulation. Fix where cheap; document where
deferred.

## Process
1. This plan reviewed by gpt-5.6-sol via `codex exec` before implementation.
2. Each fix: red test → fix → green → mutation-check → full `test-both-modes`.
3. Commit per finding (or per tight cluster) with the −/+ test and the
   mutation result in the message.
4. Re-run the round-3 escape/crash repros (shmat-unmap, dev-cloexec-exec,
   mixer-poll, TOCTOU, two-process abstract bind, compute-loop) as
   end-to-end acceptance after the HIGH fixes land.

---

# ADDENDUM — revisions after gpt-5.6-sol review (codex)

The reviewer (static audit, no tests run) found the plan "should not be
implemented as written": H1/H5/H6/H7 materially incomplete, H2 creates
pt-pool exhaustion, H3/H4 essentially correct. Two of its premises I checked
empirically and one was wrong; the corrected designs below supersede the
originals.

## Platform facts verified (reviewer was right on both)
- `O_RESOLVE_BENEATH` (0x1000), `AT_RESOLVE_BENEATH` (0x2000), `O_SEARCH`
  ARE available in this SDK, and O_RESOLVE_BENEATH works: allows interior
  files AND legitimate interior symlinks, blocks outward symlink / `..` /
  absolute — kernel-enforced, race-free. My O_NOFOLLOW_ANY approach was
  wrong (breaks legal interior symlinks). **H5 now uses a bind-root fd +
  openat(rootfd, rel, O_RESOLVE_BENEATH).**

## Reviewer premise that was WRONG (trust the measurement)
- V2: reviewer claims "a vvar write-fault test already passes" from the
  block-level RX descriptor. FALSE — the first 2MB block is L3-split, and
  EL0 WRITE to [vvar] (0xE000) SUCCEEDS (re-confirmed). The pt-pool is also
  EL0-RW. Keep the empirical finding; the low-block L3 pages need explicit
  EL0-none/EL0-RO perms.

## Corrected designs
- **H1**: drop the region-table check (not authoritative — misses high-VA
  mmap stored at GPA, kbuf alias, table-full untracked). Instead PREFLIGHT
  the page tables: walk the L2 entries across [va, va+ipa_span); if ANY is
  already valid, reject with EINVAL BEFORE mapping anything (transactional,
  authoritative). Add overflow/canonical/TTBR0-range checks. Reject
  SHM_REMAP explicitly. This also removes the non-transactional cleanup —
  nothing is mutated on the reject path.
- **H2**: collapse + RECLAIM the L3 page via a pt-pool free list (else <240
  attach/mprotect/detach cycles exhaust the 960KB bump pool). Only collapse
  an exactly-owned SHM window. Stress test past pool capacity + two live
  aliases.
- **H3**: reuse the single fd_object ownership whitelist
  (hl_fd_detached_finish) instead of a 3rd copy; fix close_range too.
  Host-unit alloc-counter leak check, not RSS.
- **H4**: guard is correct as planned (`--ret; if(ret==0 && !force_immediate)
  pure_wake`). Add a pselect6 test. V12 epoll: the last-worker trigger bumps
  the futex epoch first, so it exercises epoch-EINTR not wake-only — use a
  wake-only seam.
- **H5**: retain an O_DIRECTORY fd per bind root; express paths relative to
  it; openat(rootfd, rel, O_RESOLVE_BENEATH|O_SEARCH) for the parent/target;
  fd-based following mutations (fchmod/fchown/futimens/ftruncate/f*xattr);
  both root-relative for rename/link. Kernel-enforced, so no F_GETPATH
  re-check and no process-wide mutex needed for the same-process race.
  Residual (documented): the pre-existing inotify/bind raw-path bypass is
  VFS-F2, fixed separately; abstract names bypass VFS.
- **H6**: drop connect-probe (a live bound-not-listening socket returns
  ECONNREFUSED → would be misclassified stale and stolen). Use a per-name
  nonblocking flock held for the bound socket's lifetime (across dup/fork);
  only the lock owner may unlink a stale stand-in. Injective naming: strong
  digest over the exact counted bytes + a sidecar recording the original
  bytes for collision resolution. Restore directory self-heal.
- **H7**: PC/register progress is unsound (2-instruction oscillator evades;
  legit spin-wait dies) — DROP it. Default the watchdog OFF; make a hard
  wall-clock budget explicitly opt-in (--timeout N, N>0 arms it; absent/0 =
  off). strtol parsing (reject junk/negative/overflow). Propagate through
  fork_ipc.c child argv (grow the 8-slot array) and sys_clone_fork. Fix
  hl.1.
- **MEDIUM deltas**: V15 use a NEUTRAL metadata callback, never route statx
  through OSS ops->fstat (writes linux_stat_t, wrong layout). V13 make the
  helper PATH-AWARE (it currently has no path arg, can't tell absolute).
  V14 use renameatx_np with both dirfds; drop the non-atomic fallback. V7
  preserve the real playback deadline via an absolute CLOCK_MONOTONIC wait
  (NOT a 6s cap, which drops valid audio) and land V9 (return the failure).
  V17 make hl_vdir_t a refcounted open-file object with path+snapshot+cursor.
  V26 extract a shared redact-and-escape helper (calling hl_tracev adds
  gating/truncation); also sanitize crash `detail`.
