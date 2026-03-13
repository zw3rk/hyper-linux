# nix/lib.nix — Shared derivation builders for hyper-linux guest binaries.
#
# Copyright (c) Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Eliminates repetition across aarch64/x86_64 x musl/glibc variants.
# Each builder is parameterized by the cross package set and
# arch-specific details (linker name, assembly files, etc.).

{ linuxPkgs }:

let
  patchelf = linuxPkgs.patchelf;

  # Shell script fragment: strip nix store RPATHs from $out/lib and
  # $out/libexec so the dynamic linker resolves deps from the sysroot.
  stripRpathScript = ''
    chmod -R u+w $out/lib
    for f in $out/lib/*.so*; do
      [ -L "$f" ] && continue
      patchelf --remove-rpath "$f" 2>/dev/null || true
    done
    chmod -R u+w $out/libexec 2>/dev/null || true
    for f in $out/libexec/coreutils/*.so*; do
      [ -L "$f" ] && continue
      patchelf --remove-rpath "$f" 2>/dev/null || true
    done
  '';

in {

  # ── Sysroots ─────────────────────────────────────────────────────

  # Build a musl sysroot: dynamic linker + libc + coreutils deps
  # (acl, attr, gmp), GHC RTS deps (libffi, libnuma), GCC runtime
  # (libgcc_s, libatomic), and stdbuf.
  mkMuslSysroot = { name, crossPkgs }:
    let gccLibs = crossPkgs.stdenv.cc.cc.lib;
    in linuxPkgs.runCommand name {
      nativeBuildInputs = [ patchelf ];
    } ''
      mkdir -p $out/lib
      cp -a  ${crossPkgs.musl}/lib/ld-musl-*.so.1              $out/lib/
      cp -a  ${crossPkgs.musl}/lib/libc.so                      $out/lib/
      # coreutils: acl, attr, gmp
      cp -aL ${crossPkgs.acl.out}/lib/libacl.so*                $out/lib/
      cp -aL ${crossPkgs.attr.out}/lib/libattr.so*              $out/lib/
      cp -aL ${crossPkgs.gmp}/lib/libgmp.so*                    $out/lib/
      # GHC RTS: libffi, libnuma
      cp -aL ${crossPkgs.libffi}/lib/libffi.so*                 $out/lib/
      cp -aL ${crossPkgs.numactl}/lib/libnuma.so*               $out/lib/
      # GCC runtime: libatomic, libgcc_s
      cp -aL ${gccLibs}/lib/libatomic.so*                       $out/lib/
      cp -aL ${gccLibs}/lib/libgcc_s.so*                        $out/lib/
      # stdbuf support
      mkdir -p $out/libexec/coreutils
      cp -aL ${crossPkgs.coreutils}/libexec/coreutils/libstdbuf.so $out/libexec/coreutils/ || true

      ${stripRpathScript}
    '';

  # Build a glibc sysroot: dynamic linker + glibc shared libs + NSS
  # modules, GCC runtime, coreutils deps, stdbuf, and /lib64 symlink.
  mkGlibcSysroot = { name, pkgs, linkerName }:
    let gccLibs = pkgs.stdenv.cc.cc.lib;
    in linuxPkgs.runCommand name {
      nativeBuildInputs = [ patchelf ];
    } ''
      mkdir -p $out/lib $out/lib64
      # Dynamic linker
      cp -aL ${pkgs.glibc}/lib/${linkerName}                    $out/lib/
      # Core glibc shared libs
      cp -aL ${pkgs.glibc}/lib/libc.so.6                        $out/lib/
      cp -aL ${pkgs.glibc}/lib/libc.so                          $out/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libm.so.6                        $out/lib/
      cp -aL ${pkgs.glibc}/lib/libm.so                          $out/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libpthread.so.0                  $out/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libdl.so.2                       $out/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libresolv.so.2                   $out/lib/ || true
      cp -aL ${pkgs.glibc}/lib/librt.so.1                       $out/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libmvec.so*                      $out/lib/ || true
      # NSS modules (needed for getpwnam, getgrnam, etc.)
      cp -aL ${pkgs.glibc}/lib/libnss_files.so*                 $out/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libnss_dns.so*                   $out/lib/ || true
      # GCC runtime: libgcc_s, libatomic
      cp -aL ${gccLibs}/lib/libgcc_s.so*                        $out/lib/
      cp -aL ${gccLibs}/lib/libatomic.so*                       $out/lib/ || true
      # Coreutils deps: acl, attr, gmp
      cp -aL ${pkgs.acl.out}/lib/libacl.so*                     $out/lib/
      cp -aL ${pkgs.attr.out}/lib/libattr.so*                   $out/lib/
      cp -aL ${pkgs.gmp}/lib/libgmp.so*                         $out/lib/
      # stdbuf support
      mkdir -p $out/libexec/coreutils
      cp -aL ${pkgs.coreutils}/libexec/coreutils/libstdbuf.so $out/libexec/coreutils/ || true
      # /lib64 symlink for binaries with /lib64 PT_INTERP
      ln -sf ../lib/${linkerName} $out/lib64/${linkerName}

      ${stripRpathScript}
    '';

  # ── Test binaries ────────────────────────────────────────────────

  # Build static test binaries from test/*.c + arch-specific assembly.
  # asmFile/asmObj/ldScript vary per architecture; extraSkip lists
  # additional test files to exclude (e.g., hello-dynamic for x86_64).
  mkTestBinaries = { pname, stdenv, asmFile, asmObj, ldScript, extraSkip ? [] }:
    let
      skipChecks = builtins.concatStringsSep "\n"
        (map (s: ''[ "$name" = "${s}" ] && continue'')
          ([ "test-multi-vcpu" "test-rwx" ] ++ extraSkip));
    in stdenv.mkDerivation {
      inherit pname;
      version = "0.1.0";
      src = ../test;
      dontConfigure = true;
      dontFixup = true;
      buildPhase = ''
        $AS -o ${asmObj} ${asmFile}
        $LD -T ${ldScript} -o test-hello ${asmObj}

        for f in *.c; do
          name="''${f%.c}"
          ${skipChecks}

          extra_flags=""
          [ "$name" = "test-pthread" ] && extra_flags="-lpthread"

          $CC -static -O2 -o "$name" "$f" $extra_flags
        done
      '';
      installPhase = ''
        mkdir -p $out/bin
        cp test-hello $out/bin/
        for f in *.c; do
          name="''${f%.c}"
          [ -f "$name" ] && cp "$name" $out/bin/
        done
      '';
    };

  # Build dynamically-linked test binary from hello-dynamic.c.
  # Identical across all 4 variants (aarch64/x86_64 x musl/glibc).
  mkDynamicTestBinaries = { pname, stdenv }:
    stdenv.mkDerivation {
      inherit pname;
      version = "0.1.0";
      src = ../test;
      dontConfigure = true;
      dontFixup = true;
      buildPhase = ''
        $CC -O2 -o hello-dynamic hello-dynamic.c
        file hello-dynamic | grep -q "dynamically linked" || {
          echo "ERROR: hello-dynamic is not dynamically linked!"
          exit 1
        }
      '';
      installPhase = ''
        mkdir -p $out/bin
        cp hello-dynamic $out/bin/
      '';
    };

  # Build static bins collection (bash, lua, jq, etc.) for integration testing.
  # diffutils excluded from x86_64-musl (static build fails its test suite).
  mkStaticBins = { name, crossPkgs, includeDiffutils ? true }:
    let s = crossPkgs.pkgsStatic;
    in linuxPkgs.runCommand name {} (''
      mkdir -p $out/bin
      ln -s ${s.bash}/bin/bash          $out/bin/bash
      ln -s ${s.dash}/bin/dash           $out/bin/dash
      ln -s ${s.lua5_4}/bin/lua          $out/bin/lua
      ln -s ${s.gawk}/bin/gawk           $out/bin/gawk
      ln -s ${s.gnugrep}/bin/grep        $out/bin/grep
      ln -s ${s.gnused}/bin/sed          $out/bin/sed
      ln -s ${s.findutils}/bin/find      $out/bin/find
      ln -s ${s.tree}/bin/tree           $out/bin/tree
      ln -s ${s.jq}/bin/jq               $out/bin/jq
      ln -s ${s.sqlite}/bin/sqlite3      $out/bin/sqlite3
    '' + linuxPkgs.lib.optionalString includeDiffutils ''
      ln -s ${s.diffutils}/bin/diff      $out/bin/diff
    '');

  # ── Haskell ──────────────────────────────────────────────────────

  # Build Haskell hello-hyper test binary (static musl, cross-compiled).
  mkHaskellHello = { crossPkgs, lib }:
    crossPkgs.haskellPackages.mkDerivation {
      pname = "hello-hyper";
      version = "0.1.0";
      src = ../test/haskell;
      isExecutable = true;
      isLibrary = false;
      enableSharedExecutables = false;
      enableSharedLibraries = false;
      license = lib.licenses.asl20;
    };

  # Build Haskell integration test binaries (pandoc + shellcheck).
  # Built natively (not cross-compiled) because GHC Template Haskell
  # can't cross-compile.  Binaries keep original nix interpreter/RPATH.
  mkHaskellBins = { name, pkgs }:
    let
      hlib = pkgs.haskell.lib;
      hpkgs = pkgs.haskellPackages;
      pandoc-cli = hpkgs.pandoc-cli;
      pandoc-data = hpkgs.pandoc.data;
      shellcheck = hlib.justStaticExecutables hpkgs.ShellCheck;
    in pkgs.runCommand name {} ''
      mkdir -p $out/bin $out/share/pandoc-data
      cp -L ${pandoc-cli}/bin/pandoc $out/bin/pandoc
      cp -L ${shellcheck}/bin/shellcheck $out/bin/shellcheck
      # pandoc data files — nix pandoc compiles in a nix store path for
      # data-files lookup, so non-NixOS hosts need --data-dir at runtime.
      datadir=$(dirname $(find ${pandoc-data}/share -name abbreviations | head -1))
      cp -rL "$datadir"/. $out/share/pandoc-data/
    '';

  # Build CI haskell bundle: haskellBins + haskellHello with patched
  # interpreters and a glibc sysroot containing all shared lib deps.
  # Uploaded as CI artifact for the macOS test job to download.
  #
  # pandoc/shellcheck are dynamically linked against glibc.  Their nix
  # store interpreter paths don't exist on the macOS CI runner, so we:
  # 1. Walk RPATH of each binary to discover all shared lib deps
  # 2. Copy explicit baseline deps (glibc, GCC, GMP, libffi) as safety net
  # 3. Patch interpreters to standard /lib/ paths
  # 4. Strip RPATHs so the dynamic linker resolves from the sysroot
  mkHaskellBundle = { name, pkgs, haskellBins, haskellHello, interpreter }:
    let
      glibcLinkerName =
        if pkgs.stdenv.hostPlatform.isAarch64
        then "ld-linux-aarch64.so.1"
        else "ld-linux-x86-64.so.2";
      gccLibs = pkgs.stdenv.cc.cc.lib;
    in pkgs.runCommand name {
      nativeBuildInputs = [ pkgs.patchelf ];
    } ''
      mkdir -p $out/{haskell-bins/bin,haskell-bins/share/pandoc-data}
      mkdir -p $out/{haskell-sysroot/lib,haskell-sysroot/lib64}
      cp -rL ${haskellBins}/bin/.       $out/haskell-bins/bin/
      cp -rL ${haskellBins}/share/pandoc-data/. $out/haskell-bins/share/pandoc-data/

      # Auto-discover shared lib deps by walking RPATH of binaries AND
      # their transitive library deps.  Iterates until no new libs are
      # found (fixpoint = full transitive closure).  Uses patchelf
      # --print-rpath which works cross-arch (unlike ldd).
      chmod -R u+w $out/haskell-bins/bin/
      prev_count=-1
      while true; do
        curr_count=$(find $out/haskell-sysroot/lib -name '*.so*' 2>/dev/null | wc -l)
        [ "$curr_count" -eq "$prev_count" ] && break
        prev_count=$curr_count
        for f in $out/haskell-bins/bin/* $out/haskell-sysroot/lib/*.so*; do
          [ -f "$f" ] || continue
          rpath=$(patchelf --print-rpath "$f" 2>/dev/null) || continue
          [ -z "$rpath" ] && continue
          IFS=: read -ra dirs <<< "$rpath"
          for dir in "''${dirs[@]}"; do
            [ -d "$dir" ] || continue
            for lib in "$dir"/*.so*; do
              [ -f "$lib" ] || [ -L "$lib" ] || continue
              cp -anL "$lib" $out/haskell-sysroot/lib/ 2>/dev/null || true
            done
          done
        done
        chmod -R u+w $out/haskell-sysroot/lib 2>/dev/null || true
      done

      # Explicit baseline: glibc dynamic linker + core shared libs.
      # These may not appear in RPATH (resolved by the linker itself).
      cp -aL ${pkgs.glibc}/lib/${glibcLinkerName}            $out/haskell-sysroot/lib/
      cp -aL ${pkgs.glibc}/lib/libc.so.6                    $out/haskell-sysroot/lib/
      cp -aL ${pkgs.glibc}/lib/libc.so                      $out/haskell-sysroot/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libm.so.6                    $out/haskell-sysroot/lib/
      cp -aL ${pkgs.glibc}/lib/libm.so                      $out/haskell-sysroot/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libpthread.so.0              $out/haskell-sysroot/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libdl.so.2                   $out/haskell-sysroot/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libresolv.so.2               $out/haskell-sysroot/lib/ || true
      cp -aL ${pkgs.glibc}/lib/librt.so.1                   $out/haskell-sysroot/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libmvec.so*                  $out/haskell-sysroot/lib/ || true
      # NSS modules (needed for getpwnam, getgrnam, etc.)
      cp -aL ${pkgs.glibc}/lib/libnss_files.so*             $out/haskell-sysroot/lib/ || true
      cp -aL ${pkgs.glibc}/lib/libnss_dns.so*               $out/haskell-sysroot/lib/ || true
      # GCC runtime
      cp -aL ${gccLibs}/lib/libgcc_s.so*                    $out/haskell-sysroot/lib/
      cp -aL ${gccLibs}/lib/libatomic.so*                   $out/haskell-sysroot/lib/ || true
      # GHC RTS deps
      cp -aL ${pkgs.gmp}/lib/libgmp.so*                     $out/haskell-sysroot/lib/
      cp -aL ${pkgs.libffi}/lib/libffi.so*                  $out/haskell-sysroot/lib/
      cp -aL ${pkgs.numactl}/lib/libnuma.so*                $out/haskell-sysroot/lib/ || true

      # /lib64 symlink for binaries with /lib64 PT_INTERP
      ln -sf ../lib/${glibcLinkerName} $out/haskell-sysroot/lib64/${glibcLinkerName}

      # Strip RPATHs from sysroot libs so they resolve from sysroot
      chmod -R u+w $out/haskell-sysroot/lib
      for f in $out/haskell-sysroot/lib/*.so*; do
        [ -L "$f" ] && continue
        patchelf --remove-rpath "$f" 2>/dev/null || true
      done

      # Patch haskell binary interpreters to standard path and strip RPATHs
      for f in $out/haskell-bins/bin/*; do
        [ -f "$f" ] || continue
        if patchelf --print-interpreter "$f" 2>/dev/null | grep -q nix; then
          patchelf --set-interpreter /lib/${glibcLinkerName} "$f"
        fi
        patchelf --remove-rpath "$f" 2>/dev/null || true
      done

      # haskell-hello (static musl — only patch interpreter)
      mkdir -p $out/haskell-hello/bin
      cp -rL ${haskellHello}/bin/.         $out/haskell-hello/bin/
      chmod -R u+w $out/haskell-hello/
      for f in $out/haskell-hello/bin/*; do
        if patchelf --print-interpreter "$f" 2>/dev/null | grep -q nix; then
          patchelf --set-interpreter ${interpreter} "$f"
        fi
      done
    '';
}
