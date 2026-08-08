# Implementation report: PR0 / WS0 — Baseline, fixture, observability

## SHAs

- Start: `a34e7935fe6b08b5ce081f98b51ae4089f3da8b3` (`v0.2.4-2-ga34e793`)
- Branch: `feature/xmms-audio-vfs`

## Files added

- `src/trace.c`, `src/trace.h` — category tracing (`fs,fd,dev,audio,proc,fork`)
- `test/fixtures/xmms/MANIFEST.md`, `REPRO.md`, `baseline/README.md`
- `test/fixtures/xmms/media/smoke-440-660.wav` (+ sha256)
- Guest repro tests: `test-dev-dsp-presence.c`, `test-oss-open.c`,
  `test-vfs-chdir-relative-open.c`

## Behavior

- `HL_TRACE` / `--trace=` enable structured category logs (off by default).
- Unknown categories error clearly.
- Fixture pin documents XMMS 1.2.11 via `nix/xmms.nix` and WAV smoke media.

## Commands

```bash
make hl
make test-host-units
# guest (after nix develop / cross):
./_build/hl ./_build/test-dev-dsp-presence
./_build/hl --audio-backend null ./_build/test-oss-open
```

## Status

WS0 deliverables landed alongside subsequent PR work on the same branch
(dependency-ordered modules also started in-tree for continuous integration).
