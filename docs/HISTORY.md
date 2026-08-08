# History notes (hyper-linux)

## Published baseline

- **Remote `origin/master`** and tag **`v0.2.4`** (`41cea39`) are the
  locked public line. Do not force-push them.

## Core-only rewrite (local, 2026-07-30 onward)

Branch **`split/hl-only`** established the core-only ancestry after
`origin/master`. It is now an ancestor of local `master`; subsequent commits
contain the runtime implementation, adversarial review fixes, regressions and
release hardening for 0.3.

AppKit X / XMMS product history is **not** in this branch’s ancestry after
the rewrite.

### Archaeology (local only; not published)

| Ref | Contents |
|-----|----------|
| `split-base-2026-07-30` | Full monorepo freeze (pre-slim tip) |
| `backup/inflight-pre-rewrite` | Pre-rewrite tip (cleaned tree + long mixed history) |
| `explore/hl-appkit-rootless-spike` | Pre-split spike tip (freeze) |

### Local `master`

Local `master` intentionally advances the core-only line beyond the published
`v0.2.4` baseline. AppKit X and XMMS product work remain outside its ancestry.
Publish only through a reviewed release-preparation commit and explicit tag;
never force-push the public baseline.
