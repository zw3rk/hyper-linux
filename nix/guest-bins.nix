# nix/guest-bins.nix — All cross-compiled guest binary derivations.
#
# Copyright (c) Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Produces aarch64 and x86_64 test binaries, sysroots, coreutils,
# busybox, static bins, and the CI guest bundle.

{ linuxPkgs, crossPkgs, x64CrossPkgs, glibcCrossPkgs, x64GlibcPkgs }:

let
  helpers = import ./lib.nix { inherit linuxPkgs; };

in rec {

  # ── aarch64-linux-musl ───────────────────────────────────────────

  testBinaries = helpers.mkTestBinaries {
    pname = "hl-test-binaries";
    stdenv = crossPkgs.stdenv;
    asmFile = "hello.S";
    asmObj = "hello.o";
    ldScript = "simple.ld";
  };

  coreutils = crossPkgs.pkgsStatic.coreutils;
  busybox   = crossPkgs.pkgsStatic.busybox;

  staticBins = helpers.mkStaticBins {
    name = "hl-static-bins";
    inherit crossPkgs;
  };

  muslSysroot = helpers.mkMuslSysroot {
    name = "musl-sysroot";
    inherit crossPkgs;
  };

  dynamicTestBinaries = helpers.mkDynamicTestBinaries {
    pname = "hl-dynamic-test-binaries";
    stdenv = crossPkgs.stdenv;
  };

  dynamicCoreutils = crossPkgs.coreutils;

  # ── aarch64-linux-gnu (glibc) ───────────────────────────────────

  glibcSysroot = helpers.mkGlibcSysroot {
    name = "glibc-sysroot";
    pkgs = glibcCrossPkgs;
    linkerName = "ld-linux-aarch64.so.1";
  };

  glibcDynamicTestBinaries = helpers.mkDynamicTestBinaries {
    pname = "hl-glibc-dynamic-test-binaries";
    stdenv = glibcCrossPkgs.stdenv;
  };

  glibcDynamicCoreutils = glibcCrossPkgs.coreutils;

  # ── x86_64-linux-musl ───────────────────────────────────────────

  x64TestBinaries = helpers.mkTestBinaries {
    pname = "hl-x64-test-binaries";
    stdenv = x64CrossPkgs.stdenv;
    asmFile = "hello-x86_64.S";
    asmObj = "hello-x86_64.o";
    ldScript = "simple-x86_64.ld";
    extraSkip = [ "hello-dynamic" ];
  };

  x64Coreutils = x64CrossPkgs.pkgsStatic.coreutils;
  x64Busybox   = x64CrossPkgs.pkgsStatic.busybox;

  x64StaticBins = helpers.mkStaticBins {
    name = "hl-x64-static-bins";
    crossPkgs = x64CrossPkgs;
    includeDiffutils = false;
  };

  x64MuslSysroot = helpers.mkMuslSysroot {
    name = "x64-musl-sysroot";
    crossPkgs = x64CrossPkgs;
  };

  x64MuslDynamicTestBinaries = helpers.mkDynamicTestBinaries {
    pname = "hl-x64-musl-dynamic-test-binaries";
    stdenv = x64CrossPkgs.stdenv;
  };

  x64MuslDynamicCoreutils = x64CrossPkgs.coreutils;

  # ── x86_64-linux-gnu (glibc) ────────────────────────────────────

  x64GlibcSysroot = helpers.mkGlibcSysroot {
    name = "x64-glibc-sysroot";
    pkgs = x64GlibcPkgs;
    linkerName = "ld-linux-x86-64.so.2";
  };

  x64GlibcDynamicTestBinaries = helpers.mkDynamicTestBinaries {
    pname = "hl-x64-glibc-dynamic-test-binaries";
    stdenv = x64GlibcPkgs.stdenv;
  };

  x64GlibcDynamicCoreutils = x64GlibcPkgs.coreutils;

  # ── CI guest bundle ──────────────────────────────────────────────
  # Self-contained archive of all cross-compiled guest binaries.
  # Deep-copies (dereferences symlinks) so the output has no nix
  # store references — safe to tar up and transfer as a CI artifact.
  guestBundle = linuxPkgs.runCommand "hl-guest-bundle" {} ''
    # aarch64-linux-musl
    mkdir -p $out/{test-binaries,coreutils,busybox,static-bins}/bin
    mkdir -p $out/{sysroot/lib,dynamic-tests,dynamic-coreutils}/bin
    cp -rL ${testBinaries}/bin/.         $out/test-binaries/bin/
    cp -rL ${coreutils}/bin/.            $out/coreutils/bin/
    cp -rL ${busybox}/bin/.              $out/busybox/bin/
    cp -rL ${staticBins}/bin/.           $out/static-bins/bin/
    cp -rL ${muslSysroot}/lib/.          $out/sysroot/lib/
    cp -rL ${dynamicTestBinaries}/bin/.  $out/dynamic-tests/bin/
    cp -rL ${dynamicCoreutils}/bin/.     $out/dynamic-coreutils/bin/

    # x86_64-linux-musl
    mkdir -p $out/{x64-test-binaries,x64-coreutils,x64-busybox,x64-static-bins}/bin
    cp -rL ${x64TestBinaries}/bin/.          $out/x64-test-binaries/bin/
    cp -rL ${x64Coreutils}/bin/.             $out/x64-coreutils/bin/
    cp -rL ${x64Busybox}/bin/.               $out/x64-busybox/bin/
    cp -rL ${x64StaticBins}/bin/.            $out/x64-static-bins/bin/

    # x86_64-linux-musl dynamic
    mkdir -p $out/{x64-musl-sysroot/lib,x64-musl-dynamic-tests,x64-musl-dynamic-coreutils}/bin
    cp -rL ${x64MuslSysroot}/lib/.                $out/x64-musl-sysroot/lib/
    cp -rL ${x64MuslDynamicTestBinaries}/bin/.     $out/x64-musl-dynamic-tests/bin/
    cp -rL ${x64MuslDynamicCoreutils}/bin/.        $out/x64-musl-dynamic-coreutils/bin/

    # aarch64-linux-gnu (glibc)
    mkdir -p $out/{glibc-sysroot/lib,glibc-sysroot/lib64,glibc-dynamic-tests,glibc-dynamic-coreutils}/bin
    cp -rL ${glibcSysroot}/lib/.               $out/glibc-sysroot/lib/
    cp -rL ${glibcSysroot}/lib64/.              $out/glibc-sysroot/lib64/ || true
    cp -rL ${glibcDynamicTestBinaries}/bin/.    $out/glibc-dynamic-tests/bin/
    cp -rL ${glibcDynamicCoreutils}/bin/.       $out/glibc-dynamic-coreutils/bin/

    # x86_64-linux-gnu (glibc)
    mkdir -p $out/{x64-glibc-sysroot/lib,x64-glibc-sysroot/lib64,x64-glibc-dynamic-tests,x64-glibc-dynamic-coreutils}/bin
    cp -rL ${x64GlibcSysroot}/lib/.               $out/x64-glibc-sysroot/lib/
    cp -rL ${x64GlibcSysroot}/lib64/.              $out/x64-glibc-sysroot/lib64/ || true
    cp -rL ${x64GlibcDynamicTestBinaries}/bin/.    $out/x64-glibc-dynamic-tests/bin/
    cp -rL ${x64GlibcDynamicCoreutils}/bin/.       $out/x64-glibc-dynamic-coreutils/bin/
  '';
}
