# hl — Run Linux ELF Binaries on macOS Apple Silicon

`hl` core only. AppKit X and runnable applications live in public sibling
repositories:

| Repo | Role |
|------|------|
| [zw3rk/hyper-linux](https://github.com/zw3rk/hyper-linux) | **This** — runtime |
| [zw3rk/hyper-linux-x11](https://github.com/zw3rk/hyper-linux-x11) | AppKit-origin X |
| [zw3rk/hyper-linux-examples](https://github.com/zw3rk/hyper-linux-examples) | XMMS demo |

The pre-split monorepo is preserved by tag `split-base-2026-07-30`.

`hl` executes **aarch64-linux** and **x86_64-linux** ELF binaries on macOS Apple Silicon using Apple's Hypervisor.framework.
No Docker, no full VM — just a lightweight per-process virtual machine that translates Linux syscalls to macOS equivalents.

```
$ hl ./hello-linux
Hello from Linux!
```

## Features

- **172 Linux syscalls** translated to macOS equivalents
- **Static and dynamic ELF binaries** — use `--sysroot` for dynamically-linked programs
- **x86_64-linux via Rosetta** — transparent JIT/AOT translation for x86_64 ELF binaries
- **Multi-threading** — guest threads map 1:1 to host pthreads, each with its own HVF vCPU
- **fork/clone** — implemented via `posix_spawn` + IPC state serialization
- **execve** — full ELF reload with interpreter, page table rebuild, vCPU restart
- **Signal delivery** — `rt_sigaction`, `rt_sigreturn`, `SIGPIPE`, `ITIMER_REAL`
- **Socket networking** — `AF_INET`/`AF_INET6` with sockaddr/sockopt translation
- **inotify** — emulated via kqueue `EVFILT_VNODE`
- **`/proc` emulation** — `/proc/self/exe`, `/proc/self/maps`, `/proc/self/stat`, `/proc/self/fd/`
- **Rooted VFS** — `--fs-mode=rooted` with bind mounts, virtual CWD, reverse host→guest map
- **OSS audio** — `/dev/dsp` + `/dev/mixer` bridged to Core Audio (null/WAV backends for CI)
- **2MB + 4KB page tables** — automatic L3 splitting for mixed-permission regions (W^X)
- **Demand-paged memory** — up to 1TB address space, only used pages consume RAM

### Audio / VFS defaults

By default `hl` runs with a product-oriented profile:

- `--fs-mode=rooted`
- bind `$HOME` → `/home/user`
- `--guest-cwd /home/user`
- `--audio-backend coreaudio`

```bash
# Same as the defaults (home media + Core Audio)
hl ./my-linux-app

# Hermetic / no auto home bind
hl --isolated --bind /path/to/media:/media ./app

# Deterministic tests (legacy paths + silent backend)
hl --fs-mode=legacy --audio-backend null ./test-program

# Category traces: HL_TRACE=fs,fd,dev,audio,proc,fork,sys
```

## Requirements

- macOS on Apple Silicon (M1/M2/M3/M4)
- macOS 13+ (macOS 15+ recommended for 40-bit IPA / 1TB address space)
- Hypervisor.framework entitlement (`com.apple.security.hypervisor`)

## Installation

### Quick Install

```
curl -fsSL https://hyper-linux.app/install.sh | sh
```

Downloads, verifies SHA256, codesigns, and installs to `/usr/local/bin`.

### With Homebrew

```
brew install zw3rk/hyper-linux/hl
```

### From Release

Download the latest `.pkg` or `.zip` from [Releases](https://github.com/zw3rk/hyper-linux/releases):

```
# macOS installer package
sudo installer -pkg hl-v0.2.4.pkg -target /

# or unzip manually
unzip hl-v0.2.4.zip -d /usr/local/bin/
```

### With Nix

```
nix run github:zw3rk/hyper-linux
```

Or add to your flake inputs:

```nix
{
  inputs.hyper-linux.url = "github:zw3rk/hyper-linux";
}
```

### From Source

Requires the nix development shell (provides cross-toolchain, SDK, signing):

```
git clone https://github.com/zw3rk/hyper-linux.git
cd hyper-linux
nix develop -c make hl
```

The built binary is at `_build/hl`, codesigned with the Hypervisor entitlement.

## Usage

```
hl [options] <elf-binary> [args...]
```

### Options

| Flag | Description |
|------|-------------|
| `-h`, `--help` | Show man page (or brief usage) |
| `-V`, `--version` | Print version and exit |
| `-v`, `--verbose` | Verbose output (ELF loading, syscalls) |
| `-t`, `--timeout SECONDS` | Opt-in per-iteration vCPU watchdog (default: 0/off) |
| `--sysroot PATH` | Musl/glibc sysroot for dynamically-linked binaries |
| `--fs-mode legacy\|rooted` | Filesystem namespace mode (default: rooted) |
| `--bind HOST:GUEST` | Add a rooted-mode bind; accepts `:ro` and `GUEST=HOST` |
| `--guest-home PATH` | Set the guest home used by application profiles |
| `--guest-cwd PATH` | Set the initial rooted-mode virtual working directory |
| `--isolated` | Disable the automatic `$HOME` bind and default guest CWD |
| `--app-profile` | Add the fuller home/media/volume application profile |
| `--trace=CATEGORIES` | Trace `fs,fd,dev,audio,proc,fork,sys` or `all` |
| `--audio-backend NAME` | Select `null`, `null-realtime`, `wav`, or `coreaudio` |
| `--gdb PORT` | Start GDB RSP stub on TCP port (aarch64 only) |
| `--gdb-stop-on-entry` | Stop at ELF entry point and wait for GDB attach |
| `--` | Stop processing hl options |

### Examples

```bash
# Static aarch64-linux binary
hl ./my-static-binary arg1 arg2

# Dynamically-linked binary with musl sysroot
hl --sysroot /path/to/musl-sysroot ./my-dynamic-binary

# x86_64-linux binary (uses Rosetta for translation)
hl ./my-x86_64-binary

# Verbose output for debugging
hl -v ./program 2>debug.log

# GNU coreutils (static musl build)
hl /path/to/coreutils/bin/ls -la
```

## How It Works

1. `hl` creates a lightweight HVF virtual machine with a minimal EL1 shim
2. The ELF binary is loaded into guest memory with identity-mapped page tables
3. The shim provides exception vectors that forward Linux syscalls (`SVC #0`) to the host via `HVC #5`
4. The host translates each syscall to its macOS equivalent (errno values, flags, struct layouts)
5. For x86_64 binaries, Apple's Rosetta translates instructions to ARM64 at runtime

Guest memory is demand-paged — the VM address space can be up to 1TB but only
touched pages consume physical RAM.

## Building & Testing

```bash
# Enter development shell
nix develop

# Build
make hl

# Run tests
make test-both-modes       # aarch64 suite in legacy + rooted modes
make test-x64-both-modes   # x86_64/Rosetta suite in both modes
make test-host-units       # Host units + operator diagnostics
make test-coreutils        # 104 GNU coreutils tools
make test-busybox          # BusyBox applets
make test-static-bins      # Static binaries (bash, lua, jq, etc.)
make test-dynamic          # Dynamically-linked binaries
make test-dynamic-coreutils # Dynamic coreutils with sysroot
make test-haskell          # GHC-compiled Haskell binary
make test-x64-hello        # x86_64 via Rosetta

# All available targets
make help
```

Release maintainers: see [docs/RELEASING.md](docs/RELEASING.md).

## Project Structure

All source lives under `src/` (~26,000 lines of C + assembly):

| File | Purpose |
|------|---------|
| `src/hl.c` | CLI, VM setup, ELF loading entry point |
| `src/guest.c` | Guest memory, page tables, brk/mmap regions |
| `src/elf.c` | ELF64 parser, PT_LOAD/PT_INTERP, ET_DYN |
| `src/stack.c` | Linux initial stack builder (argc/argv/envp/auxv) |
| `src/syscall.c` | Syscall dispatch, FD table, errno translation |
| `src/syscall_fs.c` | Filesystem: stat, open, directory ops |
| `src/syscall_fd.c` | File descriptor ops: dup, dup3, fcntl, pipe2 |
| `src/syscall_io.c` | I/O: read/write, ioctl, splice, sendfile |
| `src/syscall_poll.c` | Poll/select/epoll/ppoll multiplexing |
| `src/syscall_net.c` | Socket networking, AF/sockaddr translation |
| `src/syscall_signal.c` | Signal delivery, rt_sigframe, ITIMER |
| `src/syscall_time.c` | Time: clock_gettime, nanosleep, setitimer |
| `src/syscall_sys.c` | System info: uname, getrandom, sysinfo |
| `src/syscall_inotify.c` | inotify emulation via kqueue EVFILT_VNODE |
| `src/syscall_exec.c` | execve: ELF reload, interpreter, vCPU restart |
| `src/syscall_proc.c` | Process state, wait4/waitid, ptrace, vCPU run loop |
| `src/fork_ipc.c` | fork/clone via posix_spawn + IPC |
| `src/proc_emulation.c` | /proc and /dev path interception |
| `src/rosetta.c` | Rosetta x86_64 translator setup (phase 1-2) |
| `src/crash_report.c` | Structured crash reports for GitHub issues |
| `src/vdso.c` | vDSO builder for Rosetta |
| `src/shim.S` | EL1 kernel shim, exception vectors, MMU |
| `src/thread.c` | Multi-threading, per-thread vCPU |
| `src/futex.c` | Futex wait/wake/requeue/PI |
| `src/gdb_stub.c` | GDB Remote Serial Protocol stub for debugging |

## Known Limitations

- **No full kernel environment** — no namespaces or cgroups; `/dev` is a targeted virtual registry including null/zero/random and OSS audio nodes
- **Single-VM fork** — macOS HVF allows one VM per process; fork uses `posix_spawn` + IPC serialization
- **MAP_SHARED** treated as MAP_PRIVATE (single-process, so semantically equivalent)
- **`timeout`** with dynamic linking — fork+exec with the dynamic linker has issues
- **No robust futexes** (set_robust_list stub returns 0)
- **No `clone3`** — musl falls back to `clone()`

## License

Apache License 2.0 — see [LICENSE](LICENSE).

Copyright 2025-2026 [zw3rk pte. ltd.](https://zw3rk.com)
