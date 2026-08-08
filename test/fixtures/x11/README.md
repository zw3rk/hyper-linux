# Guest X11 fixtures (hyper-linux)

Fixtures that belong to the **hl** runtime:

| Path | Purpose |
|------|---------|
| `libno_xshm.c` / `.nix` | Optional disable MIT-SHM for testing |
| `hl-xprobe.c` / `.nix` | PutImage / SHM paint probe guest binary |
| `default-xprobe.nix` | Default probe packaging helper |

X traffic uses the normal Linux AF_UNIX `DISPLAY` / `/tmp/.X11-unix` path
through hl’s socket/poll handlers. AppKit X packaging lives in
**hyper-linux-x11**.
