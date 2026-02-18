# hyper-linux Usage

## Prerequisites

- macOS on Apple Silicon (aarch64-darwin)
- Nix with flakes (provides cross-compiler and tools)

## Quick Start

```bash
# Enter development shell
nix develop

# Build the hl executable
make hl

# Run a static aarch64-linux ELF binary
_build/hl ./my-program [args...]

# Run with verbose syscall tracing
_build/hl -v ./my-program

# Run with custom timeout (default 10s)
_build/hl -t 30 ./my-program
```

## Build Targets

```bash
make help              # Show all targets
make hl                # Build + codesign hl
make test-hello        # Build and run assembly hello world
make test-all          # Run full test suite (13 tests)
make all               # Build everything (hl + all test binaries)
make clean             # Remove _build/
```

## Running Programs

```bash
# Assembly hello world
_build/hl _build/test-hello

# Musl C hello world
_build/hl _build/hello-musl

# Echo arguments
_build/hl _build/echo-test hello world

# Read a file (first 3 lines + line count)
_build/hl _build/test-fileio CLAUDE.md

# List a directory
_build/hl _build/test-ls test/

# Cat a file
_build/hl _build/test-cat test/hello.S
```

## Cross-Compiling Test Programs

```bash
# Inside nix develop shell:
aarch64-unknown-linux-musl-gcc -static -O2 -o _build/myprog test/myprog.c

# For math functions, add -lm:
aarch64-unknown-linux-musl-gcc -static -O2 -o _build/math test/math.c -lm

# Test binaries are also built automatically by `make all`
```

## Build Artifacts

All outputs go to `_build/`:
- `_build/hl` — Host hypervisor ELF executor
- `_build/shim.o`, `_build/shim.bin`, `_build/shim_blob.h` — EL1 kernel shim
- `_build/test-*`, `_build/hello-*`, `_build/echo-*` — Cross-compiled test binaries

## CLI Options

| Option | Description |
|--------|-------------|
| `-v`, `--verbose` | Print syscall trace to stderr |
| `-t N`, `--timeout N` | Set execution timeout in seconds (default: 10) |
| `--` | End of hl options (pass `-v` as argument to guest) |

## Troubleshooting

- **HV_DENIED (-85377017)**: Missing Hypervisor entitlement. Check `entitlements.plist`
  and signing with `codesign -d --entitlements - _build/hl`.
- **Unimplemented syscall N**: Run with `-v` to see full args. Add handler in syscall.c.
- **objcopy not found**: Use `nix develop` shell — it provides binutils.
- **Timeout (exit 124)**: Program runs too long. Use `-t 60` for a longer timeout.
