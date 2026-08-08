# Release implementation report — XMMS audio + rooted VFS

> Historical pre-review report. Later adversarial fixes and current release
> status are recorded in `CHANGELOG.md`, `review-followups.md`, and
> `round3-remediation-status.md`.

## Identity

- **Branch:** `feature/xmms-audio-vfs`
- **Baseline SHA:** `a34e793` → tip `8f7c8d6`
- **Host preflight:** see goal scratch `preflight.txt` (macOS 26.5.2, arm64, Xcode 26.6, Nix 2.35.1, Rosetta present, XQuartz at `/opt/X11/bin/Xquartz`)

## Delivered modules

| Module | Path |
|--------|------|
| Trace | `src/trace.[ch]` |
| FD core | `src/fd_object.[ch]` |
| VFS | `src/vfs.[ch]` |
| Devices | `src/device.[ch]` |
| Audio | `src/audio.[ch]`, `audio_oss.[ch]`, `audio_coreaudio.c`, `linux_oss_abi.h` |
| App open | `src/app_open.[ch]` |

## Commands run (implementer)

```text
make hl                          # OK
make test-host-units             # VFS, gain, stream, app-open, oss-abi PASS
# Existing guest bins smoke (8 tests) PASS
./_build/hl --trace=nope …       # rejects unknown category
```

## Product path

```text
hl --fs-mode=rooted --bind "$HOME:/home/user" --audio-backend coreaudio \
   --trace=fs,audio --sysroot <xmms-sysroot> <xmms> [guest-wav-path]
```

## Known limitations (Section 34)

- Capture/recording, ALSA, Pulse not in scope
- Fork audio: recreate-empty, not shared active stream
- MAP_SHARED treated as private (pre-existing)
- Full pathname mutation matrix polish remaining
- XMMS GUI E2E requires XQuartz + built fixture

## Follow-ups (Section 35)

- Complete special-FD open-file migration
- rename/link EXDEV matrix automation
- AppKit hyper-linux.app openFiles wiring
- Soak 1000× open/configure/write/reset/close under CI

## Concurrency notes

Documented in `CLAUDE.md`: fd_lock vs audio leaf locks; AQ callback restrictions.
