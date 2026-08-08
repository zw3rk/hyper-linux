# Implementation report: PR1–PR8 — FD core, VFS, devices, audio/OSS

## Interfaces introduced

### FD core (`src/fd_object.[ch]`)

- `hl_open_file_t` ref-counted open-file description + `hl_fd_ops_t`
- `hl_descriptor_t` per-descriptor host alias + FD_CLOEXEC
- `hl_fd_get` / `hl_fd_put`, `hl_fd_install`, `hl_fd_remove`, `hl_fd_dup`/`dup3`
- Legacy `fd_entry_t` extended with `.of` / `.desc` for gradual migration
- Close path: table detach under lock → descriptor release outside lock

### VFS (`src/vfs.[ch]`)

- `--fs-mode=legacy|rooted` (CLI default remains **legacy**)
- `--bind HOST:GUEST` / `GUEST=HOST`, `--guest-home`, `--guest-cwd`, `--app-profile`
- Longest-prefix mounts; virtual CWD (**no host chdir** in rooted mode)
- Parent+leaf resolve for `O_CREAT`; guest-absolute symlink restart
- `hl_vfs_host_to_guest` reverse map for Finder (WS9)

### Devices (`src/device.[ch]`)

- Registry for `/dev/*` nodes; pseudo devices + OSS registration
- Synthetic char-device stat; readdir enumeration

### Audio (`src/audio*.c`, `src/linux_oss_abi.h`)

- Manager + stream + bounded socketpair transport
- Backends: `null`, `null-realtime`, `wav`, `coreaudio` (AQ; isolated)
- Software gain only (master × L/R); never touches macOS system volume
- OSS Tier-1: RESET/SETFRAGMENT/SETFMT/STEREO/CHANNELS/SPEED/GETBLKSIZE/
  GETOSPACE/POST + write + fstat + NONBLOCK + mixer PCM/VOLUME
- Fork policy: **recreate-empty** (`fork_export` config only; no AQ/pointers)

## Lock order update

See `CLAUDE.md`: fd_lock remains #3; audio stream locks are leaf locks
(never hold fd_lock while waiting on audio space_cond). Core Audio callback:
no malloc, guest access, logging, or contended locks.

## Tests

| Test | Kind | Gate |
|------|------|------|
| `test/host/test-vfs-unit` | host | mounts, create leaf, reverse map, no host chdir |
| `test/host/test-audio-gain` | host | 50% gain + mute |
| `test-dev-dsp-presence` | guest | /dev/dsp,dsp0,mixer open + char fstat |
| `test-oss-open` | guest | Tier-1 ioctls + write + RESET |
| `test-vfs-chdir-relative-open` | guest | chdir + relative open |

## Known limitations

- Path syscall migration not complete for every mutation (rename/link matrix);
  openat/chdir rooted path is primary.
- Special FDs (eventfd/…) still use guest-fd keyed tables; open-file migration partial.
- Core Audio default off for CI; use `--audio-backend coreaudio`.
- Finder app integration (WS9) reverse-map API ready; app UI wiring pending package.
