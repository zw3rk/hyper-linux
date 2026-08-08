# Round-3 remediation — status

The round-3 adversarial review found 7 HIGH and ~10 MEDIUM defects, most of
them regressions the round-2 fixes introduced. This is the closing record.
The plan (`remediation-plan.md`) was reviewed by gpt-5.6-sol via codex before
implementation; that review corrected 5 of the 7 HIGH designs, and the
corrected designs are what shipped.

Every fix below has a −/+ regression test that FAILS when the fix alone is
reverted (mutation-verified), and the full `test-both-modes` suite (66/66 in
both fs modes) plus the host `test-diagnostics.sh` lane (10/10) stay green.

## HIGH — all fixed

| Commit | Item |
|---|---|
| `58f26e8` | H3 — CLOEXEC /dev fd aborted hl on execve → one dir-free whitelist |
| `e0c0703` | H4 — poll/select returned EINTR for ready fds (/dev/mixer) |
| `cbf5c92` | H1 — explicit shmat unmapped the guest's own memory |
| `8c112a3` | H2 — mprotect permanently disabled SHM → L3 collapse + pool reclaim |
| `1b27b9e` | H7 — watchdog killed compute/vDSO guests → default-off, opt-in |
| `81ded90` | H6 — abstract-socket name theft → per-name flock |
| `80637e8`,`4aa361e` | H5 — containment TOCTOU → O_RESOLVE_BENEATH (open + mutating ops) |

## MEDIUM — all fixed

| Commit | Item |
|---|---|
| `aab1f9b` | V6 — device fstat numbers lost across dup/dup2/F_DUPFD/fork |
| `6d6ca28` | V7 — drain wall-clock deadline; V9 — SYNC reports drain failure |
| `eed51af` | V13 — absolute-path *at ignored dirfd; V14 — renameat2 flags via dirfd |
| `47f32c7` | V15 — AT_EMPTY_PATH fstatat/statx device numbers (+ OSS neutral meta) |
| `a6193a0` | V17 — synthetic dir fd usable as a dirfd, dup-safe |
| `7750eb9` | V26 — crash-report every-occurrence redaction; V27 — env OOM guard |

## Residual — documented, NOT shipped (with rationale)

Two master-origin items are deliberately left for dedicated work rather than
shipped as risky or racy partial changes. NEITHER is a host escape — both are
guest-confinement / defense-in-depth completeness.

### V2 — [vvar] / pt-pool were EL0-writable (guest integrity) — FIXED
Fixed on branch `work/v2-vvar-hardening`. Full trace, evidence, and design:
`docs/implementation/v2-vvar-trace.md`.

The model mismatch that reverted the prior attempt is resolved: block 0's
descriptor really is AP=RO, but the shim's **W^X demand-toggle** promoted any
write-faulting RO page to RW (HVC #9, unconditional), so the RO setting was
defeated on the first write. Block 0 is also already L3-split by the existing
null-guard (`guest_invalidate_ptes(g,0,0x1000)`), so no new split logic was
needed. The fix is two parts:

- **shim** (`src/shim.S`): a permission fault with `FAR < 0x200000` (block 0)
  goes to `handle_el0_fault` (SIGSEGV) instead of the W^X toggle — nothing in
  block 0 legitimately needs a toggle. Blocks EL0 writes/execs to
  [vvar]/[vdso]/pt-pool/shim.
- **per-page perms** (`src/vdso.c vdso_harden_low_block`, called from `hl.c`
  and `syscall_exec.c`): pt-pool stage-1 invalid (EL1-only; walker uses
  stage-2), holes invalid, [vvar] EL0-RO/XN, [vdso] left RX.

Regression test `test/test-vvar-protect.c` (in `make test-all`, mutation-
verified). `test-both-modes` 66→67 per fs mode, both green. The x86_64/rosetta
lane was not run (change expected inert there; unverified).

### VFS-F2 — inotify_add_watch and bind(AF_UNIX) bypass the rooted resolver
In rooted mode `inotify_add_watch` opens, and `bind`/`connect`/`sendto`/
`sendmsg` name, the RAW guest path, so a guest can watch `/etc` or bind a
socket at a host `/tmp` path outside its bind. This is a confinement leak
(the same host paths are freely reachable in legacy mode; rooted mode is the
confinement feature it escapes).

A correct fix routes all of these through the resolver, but per the
gpt-5.6-sol review Darwin has no `bindat`, so resolving `bind` to an absolute
string reintroduces exactly the H5-style rename-swap race — strict
containment for the socket path needs a broker/sandbox or an explicitly
documented limitation. inotify can be closed cleanly (open the watch path
with O_RESOLVE_BENEATH beneath the bind root); the AF_UNIX side needs the
broker design. Left as one coherent unit rather than a half-fix.

## Verification debt (unchanged from round 2)
- The 4-mode matrix (`test/test-matrix.sh all`) was not run — the full
  `nix develop` shell fails to build an unrelated x86_64-musl GHC here, so
  everything was verified on `hl-aarch64` in both fs modes via `.#ci` plus a
  store cross-compiler. `hl-x64` (rosetta) most needs a run: H1's
  region-overlap, H2's SHM Stage-2, H5's resolver, and the poll/EINTR change
  all touch it.
- No TSan run on the concurrency-touching fixes (H4 poll, H6 flock table).
- The clone/SCM_RIGHTS failure with an open epoll fd (observed round 3) is
  still unfixed and untracked beyond this note.
