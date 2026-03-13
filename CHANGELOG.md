# Changelog

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
