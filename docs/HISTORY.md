# History notes (hyper-linux)

## Published baseline

- **Remote `origin/master`** and tag **`v0.2.4`** (`41cea39`) are the
  locked public line. Do not force-push them.

## Inflight rewrite (local, 2026-07-30)

Branch **`split/hl-only`** was rewritten so that commits **after**
`origin/master` contain only hyper-linux core work (2 commits):

1. `feat: VFS, audio/OSS, X11 bridge, SHM, vDSO, PV export (0.3.0-rc)`
2. `docs: 0.3.0-rc changelog, sibling repos, nix-linux-builder`

AppKit X / XMMS product history is **not** in this branch’s ancestry after
the rewrite.

### Archaeology (local only; not published)

| Ref | Contents |
|-----|----------|
| `split-base-2026-07-30` | Full monorepo freeze (pre-slim tip) |
| `backup/inflight-pre-rewrite` | Pre-rewrite tip (cleaned tree + long mixed history) |
| `explore/hl-appkit-rootless-spike` | Pre-split spike tip (freeze) |

### Local `master`

Aligned to `origin/master` (drops two unpublished local XMMS-only commits
that never belonged on the public line).
