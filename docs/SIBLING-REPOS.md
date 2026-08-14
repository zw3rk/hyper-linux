# Sibling repositories

Hyper Linux is split into three repositories under `zw3rk/`. The runtime and
AppKit X repositories are public. The local examples split seed is being
release-hardened before its public repository is created.

| Repo | Role |
|------|------|
| **hyper-linux** (this) | `hl` runtime only |
| [**hyper-linux-x11**](https://github.com/zw3rk/hyper-linux-x11) | AppKit-origin X server packaging |
| **hyper-linux-examples** *(publication pending)* | XMMS demos + sysroots |

This tree no longer carries AppKit X or XMMS product sources.

**History / archaeology:** see [HISTORY.md](HISTORY.md).

- Pre-split monorepo freeze: tag `split-base-2026-07-30`
- Pre-rewrite inflight tip: branch `backup/inflight-pre-rewrite`

Linux aarch64/x86_64 package builds on Darwin: see
[nix-linux-builder.md](nix-linux-builder.md).
