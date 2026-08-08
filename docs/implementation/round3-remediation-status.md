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

### V2 — [vvar] / pt-pool are EL0-writable (guest integrity)
The guest runs at EL0 but can read and write hl's stage-1 page-table pool and
the [vvar] time page (documented "RO to EL0" but mapped RW), letting it forge
its own vDSO clock and rewrite its own translation. HVF stage-2 still bounds
the host (verified: a crafted EL0 PTE produces a stage-2 permission fault, so
this is NOT a host escape) — it is a guest-integrity gap.

A post-build hardening pass (set the low block's [vvar] page EL0-RO and the
pt-pool pages EL1-only) was implemented and reverted: the low 2MB block is a
single BLOCK descriptor whose observed AP bits did not match the empirical
EL0-writability, so [vvar]/pt-pool are evidently mapped via a path this model
does not capture. Getting the shim's own page-table permissions wrong is
high-risk (the shim writes page tables at EL1 on every TLBI-extend), so this
needs the low-block mapping traced end-to-end first — split block 0 to L3,
then set [vvar] EL0-RO/XN, [vdso] EL0-RO/RX, pt-pool EL1-only, holes invalid —
under the full suite as the guard. Not a one-sitting change.

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
