# nix/haskell.nix — Haskell test binary derivations (both arches).
#
# Copyright (c) Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Produces hello-hyper (static musl, cross-compiled), pandoc+shellcheck
# (native, dynamically linked), and CI bundles for both aarch64 and
# x86_64.  Native builds are required because GHC Template Haskell
# can't cross-compile.

{ lib, linuxPkgs, crossPkgs, x64CrossPkgs, aarch64LinuxPkgs }:

let
  helpers = import ./lib.nix { inherit linuxPkgs; };

in rec {

  # ── Haskell hello-hyper (static musl, cross-compiled) ────────────

  haskellHello = helpers.mkHaskellHello {
    inherit crossPkgs lib;
  };

  x64HaskellHello = helpers.mkHaskellHello {
    crossPkgs = x64CrossPkgs;
    inherit lib;
  };

  # ── Haskell integration bins (native, pandoc + shellcheck) ───────

  haskellBins = helpers.mkHaskellBins {
    name = "hl-haskell-bins";
    pkgs = aarch64LinuxPkgs;
  };

  x64HaskellBins = helpers.mkHaskellBins {
    name = "hl-x64-haskell-bins";
    pkgs = linuxPkgs;
  };

  # ── CI haskell bundles ───────────────────────────────────────────

  haskellBundle = helpers.mkHaskellBundle {
    name = "hl-haskell-bundle";
    pkgs = aarch64LinuxPkgs;
    inherit haskellBins haskellHello;
    interpreter = "/lib/ld-musl-aarch64.so.1";
  };

  x64HaskellBundle = helpers.mkHaskellBundle {
    name = "hl-x64-haskell-bundle";
    pkgs = linuxPkgs;
    haskellBins = x64HaskellBins;
    haskellHello = x64HaskellHello;
    interpreter = "/lib/ld-musl-x86_64.so.1";
  };
}
