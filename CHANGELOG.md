# Changelog

## [Unreleased] — 0.3.0-rc (hl core only)

### Added
- Ref-counted open-file FD core (`src/fd_object.*`) with per-descriptor ops
- Rooted VFS mode: `--fs-mode`, `--bind`, `--guest-home`, `--guest-cwd`, `--app-profile`
- Virtual device registry; OSS `/dev/dsp`, `/dev/dsp0`, `/dev/audio`, `/dev/mixer`
- Audio stream manager with null / null-realtime / WAV / Core Audio backends
- Software mixer gain (never changes macOS system volume); live mixer → open DSP streams
- Category tracing: `HL_TRACE` / `--trace=fs,fd,dev,audio,proc,fork`
- Guest DISPLAY / X11 socket bridging; SysV SHM (MIT-SHM); guest vDSO text
- Guest tests: OSS/VFS/vdso/tgkill; host unit tests for VFS/FD/audio

### Removed
- Paravirtual X11 ring (`HL_X11_PV`, `libhl_x11_pv.so`, `packages.*.hl-x11-pv`) —
  high complexity, no product win; stock AF_UNIX X path is sufficient

### Notes
- CLI defaults: `--fs-mode=rooted`, bind `$HOME:/home/user`, `--guest-cwd /home/user`,
  `--audio-backend coreaudio`; `--isolated` skips auto home bind
- OSS fork policy v1: recreate-empty independent streams (no AQ pointer IPC)
- AppKit X and XMMS live in sibling repos (see `docs/SIBLING-REPOS.md`)

## [0.2.4] - 2026-03-15

### Added
- Add GDB remote serial protocol stub for aarch64 guest debugging (hardware breakpoints, watchpoints, single-step, full register/memory access, thread support)
- Add X11 raw wire protocol test (test-x11)

### Fixed
- Fix futex_unlock_pi to unlink woken waiter before signaling, preventing use-after-free
- Hold mmap_lock during fork IPC region enumeration to prevent races
- Drain oversized cmdline in fork child to prevent IPC stream desync
- Recycle SP_EL1 slots on thread exit and unlink woken futex waiters
- Handle EC=0 (undefined instruction) from EL0 gracefully instead of crashing
- Add `_GNU_SOURCE` for `REG_RIP` in test-sigill.c

### Documentation
- Add GDB stub to architecture list and debugging section in CLAUDE.md

## [0.2.3] - 2026-03-13

### Added
- Set process title to the guest binary name, making `hl` processes identifiable in `ps` and Activity Monitor

## [0.2.2] - 2026-03-13

### Added
- Emulate `/proc/self/stat` and intercept `stat()` on `/proc` paths for programs that probe process state

### Fixed
- Coalesce adjacent anonymous memory regions to prevent region table overflow
- Handle `IP_MTU_DISCOVER` setsockopt for P2P networking (e.g. libp2p)
- Translate Linux dynamic CPU clock IDs (`CLOCK_THREAD_CPUTIME_ID` per-thread variants) for GHC RTS timer support
- Fix `release.sh` to work when invoked from a Claude Code session

## [0.2.1] - 2026-03-13

### Fixed
- Preserve Hypervisor.framework entitlement in nix build (`nix run` no longer killed)
- Portable in-place sed in release script (GNU sed vs macOS sed compatibility)

## [0.2.0] - 2026-03-13

### Added
- x86_64-linux support via Apple's Rosetta Linux translator (JIT + AOT)
- Rosettad AOT persistent cache at `~/.cache/hl-rosettad/` with automatic translation
- VZ ioctl emulation for Rosetta AOT activation
- DC ZVA emulation in shim for Rosetta JIT correctness
- PI futex (FUTEX_LOCK_PI/UNLOCK_PI/TRYLOCK_PI) with dead-owner detection
- COW fork via file-backed shared memory (zero-copy for aarch64)
- clone(CLONE_VM) and ptrace for Rosetta's two-process JIT architecture
- vDSO builder for Rosetta's clock_gettime fast path
- Crash report generator for GitHub issue filing
- 4-mode test matrix (hl-aarch64, hl-x64, lima-aarch64, lima-x64)
- Haskell binary testing (pandoc, shellcheck) with glibc sysroot bundles
- Homebrew tap: `brew install zw3rk/hyper-linux/hl`
- Curl installer with SHA256 verification: `curl -fsSL https://hyper-linux.app/install.sh | sh`
- macOS .pkg installer
- Interactive release automation (`dist/release.sh`)
- Cloudflare Workers site deployment

### Changed
- Syscall count: ~140 → 172
- Stack size: 2MB → 8MB (4x2MB blocks, dynamic position)
- Address space: up to 1TB with 48-bit IPA for Rosetta mode
- Fork: COW path for aarch64 (instant, zero data copy), IPC path for Rosetta
- I-cache coherence: IC IALLU after every syscall return

### Fixed
- AT_BASE always emitted in auxv (fixes musl x86_64 SIGFPE)
- ELF page-tail zeroing (fixes glibc dl-minimal-malloc stale data)
- PROT_NONE high-VA PTE creation (fixes x86_64 PIE mmap at 85TB)
- TTBR1 kbuf re-initialization across execve
- Multi-call binary argv preservation (busybox applet dispatch)
- GHC shutdown deadlocks: PI futex dead-owner, eventfd/signalfd close races
- Blocking syscall interruptibility (-EINTR after 1s for no-timeout futex)
- Haskell sysroot RPATH discovery via fixpoint loop
- Comprehensive clang-tidy lint-clean

### Documentation
- Man page: stack size, msync implementation, missing /proc paths, COW fork
- README: project structure, install methods, syscall count
- Site: futex count, stack layout, Rosetta AOT description
