# Building aarch64-linux / x86_64-linux packages on Darwin

Hyper Linux needs **Linux** store paths (guest test bins, sysroots) while
developing on Apple Silicon. Sibling demos (XMMS) also build
`packages.aarch64-linux.*` on Darwin. Use:

**[input-output-hk/nix-linux-builder](https://github.com/input-output-hk/nix-linux-builder)**

That project registers remote builders so `nix build .#packages.aarch64-linux.…`
and `…x86_64-linux…` realize on a Linux builder from macOS.

## Check

```bash
nix show-config | grep -E 'builders|extra-platforms' || true
# After builder is up:
nix build .#packages.aarch64-linux.test-binaries -L
```

If the builder is missing, builds fail with “unable to build … aarch64-linux”
(or hang trying to download missing substitutes). Fix the builder first.

## Split repos

| Package | System | Builder |
|---------|--------|---------|
| `hl` | aarch64-darwin | local |
| `xorg-server-appkit-origin` | aarch64-darwin | local (hyper-linux-x11) |
| `xmms`, `xmms-sysroot` | aarch64-linux | nix-linux-builder (examples) |
