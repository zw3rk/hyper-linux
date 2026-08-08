# Section 30 release acceptance status

**Branch:** `feature/xmms-audio-vfs`  
**Updated:** 2026-07-24 (post-skeptic fix-up)

## Status

| Area | Status | Evidence |
|------|--------|----------|
| Rooted VFS one resolver | **DONE** | All pathname ops in `syscall_fs.c` use `resolve_path_for_op` → `hl_vfs_resolve_at` in rooted mode (open/stat/unlink/mkdir/rename/link/symlink/access/truncate/chmod/chown/utimens/xattr) |
| Virtual CWD / no host chdir | **DONE** | rooted guest test + host unit |
| Create under bind (O_CREAT) | **DONE** | `test-vfs-rooted` guest with `--fs-mode=rooted --bind` |
| Reverse host→guest | **DONE** | host `test-app-open` |
| FD open-file core | **DONE** | host `test-fd-object-lite` (create/install/get/dup/remove) |
| OSS Tier-1 + mixer | **DONE** | guest `test-oss-open` + `test-oss-tier1` (POST, NONBLOCK EAGAIN, poll, mixer, dup) |
| WAV signal content | **DONE** | host stream write → non-empty data chunk (1024B) |
| Core Audio isolation | **DONE** | worker not started for CA backend; callback-only cons_fd consumer |
| Software mixer only | **DONE** | gain unit test; mixer ioctls |
| Fork recreate-empty | **DONE** | IPC v5 wires `hl_oss_fork_export/import`; guest `test-oss-fork` PASS (child free=cap after parent write) |
| Finder argv no shell | **DONE** | `app_open` host test |
| Docs man/README/CHANGELOG/CLAUDE | **DONE** | updated |
| Four-mode matrix | **PARTIAL** | `make test-all` hl-aarch64 units: **47/0 PASS** (log matrix-hl-aarch64.log). hl-x64 blocked on host hv_vm_map for all x64. Lima modes not available in agent. |
| x64 OSS under Rosetta | **PARTIAL / ENV** | binary built; host fails `hv_vm_map` for all x64 guests (pre-existing) |
| XMMS XQuartz GUI E2E | **ENV** | fixture+REPRO; interactive display not driven in agent |

## Approved exceptions

1. Full four-mode matrix: capture aarch64 smoke; CI runs `test-matrix.sh all`.  
2. x64 guest runtime: IPA map failure on this host for all x64; ABI + aarch64 prove OSS.  
3. XMMS audible GUI: null/WAV + path gates are CI bar (plan §6).  
