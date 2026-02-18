# hyper-linux Usage

## Prerequisites

- macOS on Apple Silicon (aarch64-darwin)
- Xcode command-line tools (for `as`, `clang`, `codesign`)
- GNU binutils via nix (`objcopy` for binary blob creation)

## Build & Run

```bash
# Show available targets
make help

# Build and run (default ad-hoc signing)
make run

# Build with developer signing identity
make run SIGN_IDENTITY="Apple Development: Your Name (XXXXXXXXXX)"

# Build only (no run)
make all

# Clean build artifacts
make clean
```

## Build Artifacts

All outputs go to `_build/`:
- `_build/vm` — Host hypervisor executable
- `_build/shim.o`, `_build/shim.bin` — EL1 kernel shim
- `_build/user.o`, `_build/user.bin` — EL0 test code

## How It Works

1. `vm.c` allocates 16MB guest memory at GPA 0x80000000
2. Sets up 3-level page tables (L0 -> L1 -> L2) with 2MB block mappings
3. Loads `shim.bin` at guest base, `user.bin` at offset 0x4000
4. Creates vCPU, sets PC to guest base, CPSR to EL1h (0x3c5)
5. Shim configures system registers via HVC #4 calls to host
6. Shim enables MMU, sets up exception vectors, ERETs to EL0
7. User code runs: unaligned access, SIMD, SVC calls
8. SVC handled by shim's exception vectors, HVC forwarded to host

## Troubleshooting

- **HV_DENIED (-85377017)**: Missing Hypervisor entitlement. Check `entitlements.plist`
  and signing with `codesign -d --entitlements - _build/vm`.
- **Hang on vcpu_run**: Usually MMU misconfiguration. Check page table setup in vm.c.
- **objcopy not found**: Ensure nix is installed. The Makefile uses `nix shell nixpkgs#binutils`.

## Finding Your Signing Identity

```bash
security find-identity -v -p codesigning
```

Ad-hoc signing (`-`) works for local development.
