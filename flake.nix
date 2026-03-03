{
  description = "hl — aarch64-linux and x86_64-linux ELF executor on macOS Apple Silicon via Hypervisor.framework";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      darwinSystem = "aarch64-darwin";
      darwinPkgs = nixpkgs.legacyPackages.${darwinSystem};

      # Cross-compilation uses x86_64-linux as build platform.  Nix auto-
      # dispatches these derivations to a configured remote linux builder
      # when building on aarch64-darwin.
      linuxSystem = "x86_64-linux";
      linuxPkgs = nixpkgs.legacyPackages.${linuxSystem};
      crossPkgs = linuxPkgs.pkgsCross.aarch64-multiplatform-musl;

      # x86_64-linux-musl cross packages (for rosetta testing).
      # Since linuxSystem is x86_64-linux, musl64 cross-compiles from
      # x86_64-linux to x86_64-linux-musl (same arch, different libc).
      x64CrossPkgs = linuxPkgs.pkgsCross.musl64;

      # aarch64-linux-gnu (glibc) cross packages — for glibc dynamic linking tests.
      # Uses aarch64-multiplatform (glibc-based, NOT the -musl variant).
      glibcCrossPkgs = linuxPkgs.pkgsCross.aarch64-multiplatform;

      # x86_64-linux-gnu (glibc) packages — native x86_64-linux packages
      # (linuxPkgs IS x86_64-linux with glibc), for x86_64 glibc testing via Rosetta.
      x64GlibcPkgs = linuxPkgs;

      # Darwin SDK for Hypervisor.framework
      darwinBuildInputs = [
        darwinPkgs.apple-sdk_15
        (darwinPkgs.darwinMinVersionHook "15.0")
      ];

      # Static aarch64-linux-musl test binaries, built on x86_64-linux
      testBinaries = crossPkgs.stdenv.mkDerivation {
        pname = "hl-test-binaries";
        version = "0.1.0";
        src = ./test;

        dontConfigure = true;
        dontFixup = true;  # don't strip/patchelf — these are cross binaries

        buildPhase = ''
          # Assembly hello world (custom linker script)
          $AS -o hello.o hello.S
          $LD -T simple.ld -o test-hello hello.o

          # All C test programs (static musl).
          # Skip test-multi-vcpu.c — it's a native macOS binary (Hypervisor.framework).
          for f in *.c; do
            name="''${f%.c}"
            [ "$name" = "test-multi-vcpu" ] && continue

            # test-pthread needs -lpthread for musl static pthread support
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

      # Static aarch64-linux-musl guest tool binaries
      guestBins = {
        coreutils = crossPkgs.pkgsStatic.coreutils;
        busybox   = crossPkgs.pkgsStatic.busybox;
      };

      # Static binaries for the integration test suite (test-static-bins).
      # Each is a standalone static aarch64-linux-musl ELF, covering shells,
      # scripting languages, text tools, JSON/SQL processors, and more.
      staticBins = crossPkgs.runCommand "hl-static-bins" {} ''
        mkdir -p $out/bin
        ln -s ${crossPkgs.pkgsStatic.bash}/bin/bash          $out/bin/bash
        ln -s ${crossPkgs.pkgsStatic.dash}/bin/dash           $out/bin/dash
        ln -s ${crossPkgs.pkgsStatic.lua5_4}/bin/lua          $out/bin/lua
        ln -s ${crossPkgs.pkgsStatic.gawk}/bin/gawk           $out/bin/gawk
        ln -s ${crossPkgs.pkgsStatic.gnugrep}/bin/grep        $out/bin/grep
        ln -s ${crossPkgs.pkgsStatic.gnused}/bin/sed          $out/bin/sed
        ln -s ${crossPkgs.pkgsStatic.findutils}/bin/find      $out/bin/find
        ln -s ${crossPkgs.pkgsStatic.tree}/bin/tree           $out/bin/tree
        ln -s ${crossPkgs.pkgsStatic.jq}/bin/jq               $out/bin/jq
        ln -s ${crossPkgs.pkgsStatic.sqlite}/bin/sqlite3      $out/bin/sqlite3
        ln -s ${crossPkgs.pkgsStatic.diffutils}/bin/diff      $out/bin/diff
      '';

      # ── x86_64-linux-musl test binaries (for rosetta testing) ────

      # Static x86_64-linux-musl test binaries, built on x86_64-linux.
      # Same C sources as aarch64, plus x86_64-specific assembly.
      x64TestBinaries = x64CrossPkgs.stdenv.mkDerivation {
        pname = "hl-x64-test-binaries";
        version = "0.1.0";
        src = ./test;

        dontConfigure = true;
        dontFixup = true;

        buildPhase = ''
          # x86_64 assembly hello world (custom linker script)
          $AS -o hello-x86_64.o hello-x86_64.S
          $LD -T simple-x86_64.ld -o test-hello hello-x86_64.o

          # All C test programs (static musl) — same source as aarch64.
          # raw-syscall.h and test-signal.c provide dual-arch support via
          # #ifdef __aarch64__ / __x86_64__ for inline asm and registers.
          for f in *.c; do
            name="''${f%.c}"
            [ "$name" = "test-multi-vcpu" ] && continue   # native macOS (HVF API)
            [ "$name" = "hello-dynamic" ] && continue      # dynamic linking not tested yet

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

      # Static x86_64-linux-musl guest tool binaries
      x64GuestBins = {
        coreutils = x64CrossPkgs.pkgsStatic.coreutils;
        busybox   = x64CrossPkgs.pkgsStatic.busybox;
      };

      # Static x86_64-linux-musl binaries for rosetta integration testing.
      # Mirrors the aarch64 staticBins collection above.
      # NOTE: diffutils excluded — x86_64-musl static build fails its own
      # test suite (8/344 gnulib tests fail). The diff test in test-static-bins.sh
      # will be skipped when the binary is absent.
      x64StaticBins = x64CrossPkgs.runCommand "hl-x64-static-bins" {} ''
        mkdir -p $out/bin
        ln -s ${x64CrossPkgs.pkgsStatic.bash}/bin/bash          $out/bin/bash
        ln -s ${x64CrossPkgs.pkgsStatic.dash}/bin/dash           $out/bin/dash
        ln -s ${x64CrossPkgs.pkgsStatic.lua5_4}/bin/lua          $out/bin/lua
        ln -s ${x64CrossPkgs.pkgsStatic.gawk}/bin/gawk           $out/bin/gawk
        ln -s ${x64CrossPkgs.pkgsStatic.gnugrep}/bin/grep        $out/bin/grep
        ln -s ${x64CrossPkgs.pkgsStatic.gnused}/bin/sed          $out/bin/sed
        ln -s ${x64CrossPkgs.pkgsStatic.findutils}/bin/find      $out/bin/find
        ln -s ${x64CrossPkgs.pkgsStatic.tree}/bin/tree           $out/bin/tree
        ln -s ${x64CrossPkgs.pkgsStatic.jq}/bin/jq               $out/bin/jq
        ln -s ${x64CrossPkgs.pkgsStatic.sqlite}/bin/sqlite3      $out/bin/sqlite3
      '';

      # ── Dynamic linking test infrastructure ──────────────────────

      # Musl sysroot: dynamic linker + libc + shared libs needed by
      # dynamically-linked coreutils and GHC-compiled binaries.
      # All libs are patchelf'd to remove nix store RPATHs so the musl
      # dynamic linker resolves everything from the sysroot /lib.
      gccLibs = crossPkgs.stdenv.cc.cc.lib;
      musl-sysroot = crossPkgs.runCommand "musl-sysroot" {
        nativeBuildInputs = [ linuxPkgs.patchelf ];
      } ''
        mkdir -p $out/lib
        cp -a  ${crossPkgs.musl}/lib/ld-musl-aarch64.so.1      $out/lib/
        cp -a  ${crossPkgs.musl}/lib/libc.so                    $out/lib/
        # coreutils: acl, attr, gmp
        cp -aL ${crossPkgs.acl.out}/lib/libacl.so*              $out/lib/
        cp -aL ${crossPkgs.attr.out}/lib/libattr.so*            $out/lib/
        cp -aL ${crossPkgs.gmp}/lib/libgmp.so*                  $out/lib/
        # GHC RTS: libffi, libnuma
        cp -aL ${crossPkgs.libffi}/lib/libffi.so*               $out/lib/
        cp -aL ${crossPkgs.numactl}/lib/libnuma.so*             $out/lib/
        # GCC runtime: libatomic, libgcc_s (needed by libnuma, libffi)
        cp -aL ${gccLibs}/lib/libatomic.so*                     $out/lib/
        cp -aL ${gccLibs}/lib/libgcc_s.so*                      $out/lib/

        # Strip nix store RPATHs from all shared libs so the musl
        # dynamic linker resolves deps from /lib (the sysroot root).
        chmod -R u+w $out/lib
        for f in $out/lib/*.so*; do
          [ -L "$f" ] && continue  # skip symlinks
          patchelf --remove-rpath "$f" 2>/dev/null || true
        done
      '';

      # Dynamically-linked test binaries (linked against musl shared lib)
      dynamicTestBinaries = crossPkgs.stdenv.mkDerivation {
        pname = "hl-dynamic-test-binaries";
        version = "0.1.0";
        src = ./test;
        dontConfigure = true;
        dontFixup = true;
        buildPhase = ''
          $CC -O2 -o hello-dynamic hello-dynamic.c
          # Verify it's actually dynamically linked
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

      # Dynamically-linked coreutils (default musl, NOT pkgsStatic)
      dynamicCoreutils = crossPkgs.coreutils;

      # ── glibc dynamic linking test infrastructure ────────────────

      # aarch64-linux-gnu (glibc) sysroot: dynamic linker + shared libs
      # needed by dynamically-linked glibc coreutils.
      glibcGccLibs = glibcCrossPkgs.stdenv.cc.cc.lib;
      glibc-sysroot = glibcCrossPkgs.runCommand "glibc-sysroot" {
        nativeBuildInputs = [ linuxPkgs.patchelf ];
      } ''
        mkdir -p $out/lib $out/lib64
        # Dynamic linker
        cp -aL ${glibcCrossPkgs.glibc}/lib/ld-linux-aarch64.so.1    $out/lib/
        # Core glibc shared libs
        cp -aL ${glibcCrossPkgs.glibc}/lib/libc.so.6                $out/lib/
        cp -aL ${glibcCrossPkgs.glibc}/lib/libc.so                  $out/lib/ || true
        cp -aL ${glibcCrossPkgs.glibc}/lib/libm.so.6                $out/lib/
        cp -aL ${glibcCrossPkgs.glibc}/lib/libm.so                  $out/lib/ || true
        cp -aL ${glibcCrossPkgs.glibc}/lib/libpthread.so.0          $out/lib/ || true
        cp -aL ${glibcCrossPkgs.glibc}/lib/libdl.so.2               $out/lib/ || true
        cp -aL ${glibcCrossPkgs.glibc}/lib/libresolv.so.2           $out/lib/ || true
        cp -aL ${glibcCrossPkgs.glibc}/lib/librt.so.1               $out/lib/ || true
        cp -aL ${glibcCrossPkgs.glibc}/lib/libmvec.so*              $out/lib/ || true
        # NSS modules (needed for getpwnam, getgrnam, etc.)
        cp -aL ${glibcCrossPkgs.glibc}/lib/libnss_files.so*         $out/lib/ || true
        cp -aL ${glibcCrossPkgs.glibc}/lib/libnss_dns.so*           $out/lib/ || true
        # GCC runtime: libgcc_s, libatomic
        cp -aL ${glibcGccLibs}/lib/libgcc_s.so*                     $out/lib/
        cp -aL ${glibcGccLibs}/lib/libatomic.so*                    $out/lib/ || true
        # Coreutils deps: acl, attr, gmp
        cp -aL ${glibcCrossPkgs.acl.out}/lib/libacl.so*             $out/lib/
        cp -aL ${glibcCrossPkgs.attr.out}/lib/libattr.so*           $out/lib/
        cp -aL ${glibcCrossPkgs.gmp}/lib/libgmp.so*                 $out/lib/
        # /lib64 symlink for binaries with /lib64 PT_INTERP
        ln -sf ../lib/ld-linux-aarch64.so.1 $out/lib64/ld-linux-aarch64.so.1

        # Strip nix store RPATHs
        chmod -R u+w $out/lib
        for f in $out/lib/*.so*; do
          [ -L "$f" ] && continue
          patchelf --remove-rpath "$f" 2>/dev/null || true
        done
      '';

      # Dynamically-linked glibc test binary (aarch64)
      glibcDynamicTestBinaries = glibcCrossPkgs.stdenv.mkDerivation {
        pname = "hl-glibc-dynamic-test-binaries";
        version = "0.1.0";
        src = ./test;
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

      # Dynamically-linked glibc coreutils (aarch64)
      glibcDynamicCoreutils = glibcCrossPkgs.coreutils;

      # x86_64-linux-gnu (glibc) sysroot
      x64GlibcGccLibs = x64GlibcPkgs.stdenv.cc.cc.lib;
      x64-glibc-sysroot = x64GlibcPkgs.runCommand "x64-glibc-sysroot" {
        nativeBuildInputs = [ linuxPkgs.patchelf ];
      } ''
        mkdir -p $out/lib $out/lib64
        # Dynamic linker
        cp -aL ${x64GlibcPkgs.glibc}/lib/ld-linux-x86-64.so.2      $out/lib/
        # Core glibc shared libs
        cp -aL ${x64GlibcPkgs.glibc}/lib/libc.so.6                  $out/lib/
        cp -aL ${x64GlibcPkgs.glibc}/lib/libc.so                    $out/lib/ || true
        cp -aL ${x64GlibcPkgs.glibc}/lib/libm.so.6                  $out/lib/
        cp -aL ${x64GlibcPkgs.glibc}/lib/libm.so                    $out/lib/ || true
        cp -aL ${x64GlibcPkgs.glibc}/lib/libpthread.so.0            $out/lib/ || true
        cp -aL ${x64GlibcPkgs.glibc}/lib/libdl.so.2                 $out/lib/ || true
        cp -aL ${x64GlibcPkgs.glibc}/lib/libresolv.so.2             $out/lib/ || true
        cp -aL ${x64GlibcPkgs.glibc}/lib/librt.so.1                 $out/lib/ || true
        cp -aL ${x64GlibcPkgs.glibc}/lib/libmvec.so*                $out/lib/ || true
        # NSS modules
        cp -aL ${x64GlibcPkgs.glibc}/lib/libnss_files.so*           $out/lib/ || true
        cp -aL ${x64GlibcPkgs.glibc}/lib/libnss_dns.so*             $out/lib/ || true
        # GCC runtime
        cp -aL ${x64GlibcGccLibs}/lib/libgcc_s.so*                  $out/lib/
        cp -aL ${x64GlibcGccLibs}/lib/libatomic.so*                 $out/lib/ || true
        # Coreutils deps
        cp -aL ${x64GlibcPkgs.acl.out}/lib/libacl.so*               $out/lib/
        cp -aL ${x64GlibcPkgs.attr.out}/lib/libattr.so*             $out/lib/
        cp -aL ${x64GlibcPkgs.gmp}/lib/libgmp.so*                   $out/lib/
        # /lib64 symlink
        ln -sf ../lib/ld-linux-x86-64.so.2 $out/lib64/ld-linux-x86-64.so.2

        # Strip nix store RPATHs
        chmod -R u+w $out/lib
        for f in $out/lib/*.so*; do
          [ -L "$f" ] && continue
          patchelf --remove-rpath "$f" 2>/dev/null || true
        done
      '';

      # Dynamically-linked glibc test binary (x86_64)
      x64GlibcDynamicTestBinaries = x64GlibcPkgs.stdenv.mkDerivation {
        pname = "hl-x64-glibc-dynamic-test-binaries";
        version = "0.1.0";
        src = ./test;
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

      # Dynamically-linked glibc coreutils (x86_64)
      x64GlibcDynamicCoreutils = x64GlibcPkgs.coreutils;

      # ── Haskell test binary ────────────────────────────────────────
      # Static aarch64-linux-musl Haskell "Hello World" to verify hl
      # can run GHC-produced ELF binaries.  Built via nixpkgs cross
      # haskellPackages (GHC runs on x86_64-linux, targets aarch64-musl).
      haskellHello = crossPkgs.haskellPackages.mkDerivation {
        pname = "hello-hyper";
        version = "0.1.0";
        src = ./test/haskell;
        isExecutable = true;
        isLibrary = false;
        enableSharedExecutables = false;
        enableSharedLibraries = false;
        license = linuxPkgs.lib.licenses.asl20;
      };

      # ── Haskell integration test binaries (native aarch64-linux) ──
      # pandoc and shellcheck exercise the GHC runtime seriously:
      # threaded RTS, heavy allocation, Lua FFI (pandoc), regex
      # engines (shellcheck).  Built natively on aarch64-linux to
      # avoid cross-compilation issues (GHC external interpreter,
      # Template Haskell in pandoc's deps).  justStaticExecutables
      # produces fully-static glibc-linked binaries without the
      # pkgsStatic/isStatic numactl problem.
      aarch64LinuxPkgs = nixpkgs.legacyPackages.aarch64-linux;

      haskellBins =
        let
          hlib = aarch64LinuxPkgs.haskell.lib;
          hpkgs = aarch64LinuxPkgs.haskellPackages;
        in aarch64LinuxPkgs.runCommand "hl-haskell-bins" {} ''
          mkdir -p $out/bin
          ln -s ${hlib.justStaticExecutables hpkgs.pandoc}/bin/pandoc $out/bin/pandoc
          ln -s ${hlib.justStaticExecutables hpkgs.ShellCheck}/bin/shellcheck $out/bin/shellcheck
        '';

      # ── CI guest bundle ──────────────────────────────────────────
      # Self-contained archive of all cross-compiled guest binaries.
      # Deep-copies (dereferences symlinks) so the output has no nix
      # store references — safe to tar up and transfer as a CI artifact.
      # Excludes haskellBins (native aarch64-linux, can't build on
      # the x86_64-linux CI runner).
      guestBundle = crossPkgs.runCommand "hl-guest-bundle" {
        nativeBuildInputs = [ linuxPkgs.patchelf ];
      } ''
        mkdir -p $out/{test-binaries,coreutils,busybox,static-bins}/bin
        mkdir -p $out/{sysroot/lib,dynamic-tests,dynamic-coreutils,haskell-hello}/bin
        cp -rL ${testBinaries}/bin/.         $out/test-binaries/bin/
        cp -rL ${guestBins.coreutils}/bin/.  $out/coreutils/bin/
        cp -rL ${guestBins.busybox}/bin/.    $out/busybox/bin/
        cp -rL ${staticBins}/bin/.           $out/static-bins/bin/
        cp -rL ${musl-sysroot}/lib/.         $out/sysroot/lib/
        cp -rL ${dynamicTestBinaries}/bin/.  $out/dynamic-tests/bin/
        cp -rL ${dynamicCoreutils}/bin/.     $out/dynamic-coreutils/bin/
        cp -rL ${haskellHello}/bin/.         $out/haskell-hello/bin/

        # Patch haskell-hello interpreter to use sysroot-relative path.
        # GHC cross-musl produces dynamically-linked binaries with nix
        # store interpreter paths; rewrite to /lib/ld-musl-aarch64.so.1
        # so hl --sysroot can find the dynamic linker.
        chmod -R u+w $out/haskell-hello/
        for f in $out/haskell-hello/bin/*; do
          if patchelf --print-interpreter "$f" 2>/dev/null | grep -q nix; then
            patchelf --set-interpreter /lib/ld-musl-aarch64.so.1 "$f"
          fi
        done

        # x86_64-linux test binaries (for rosetta testing via test-x64-*)
        mkdir -p $out/{x64-test-binaries,x64-coreutils,x64-busybox,x64-static-bins}/bin
        cp -rL ${x64TestBinaries}/bin/.          $out/x64-test-binaries/bin/
        cp -rL ${x64GuestBins.coreutils}/bin/.   $out/x64-coreutils/bin/
        cp -rL ${x64GuestBins.busybox}/bin/.     $out/x64-busybox/bin/
        cp -rL ${x64StaticBins}/bin/.            $out/x64-static-bins/bin/

        # aarch64-linux-gnu (glibc) test artifacts
        mkdir -p $out/{glibc-sysroot/lib,glibc-sysroot/lib64,glibc-dynamic-tests,glibc-dynamic-coreutils}/bin
        cp -rL ${glibc-sysroot}/lib/.               $out/glibc-sysroot/lib/
        cp -rL ${glibc-sysroot}/lib64/.              $out/glibc-sysroot/lib64/ || true
        cp -rL ${glibcDynamicTestBinaries}/bin/.     $out/glibc-dynamic-tests/bin/
        cp -rL ${glibcDynamicCoreutils}/bin/.        $out/glibc-dynamic-coreutils/bin/

        # x86_64-linux-gnu (glibc) test artifacts
        mkdir -p $out/{x64-glibc-sysroot/lib,x64-glibc-sysroot/lib64,x64-glibc-dynamic-tests,x64-glibc-dynamic-coreutils}/bin
        cp -rL ${x64-glibc-sysroot}/lib/.               $out/x64-glibc-sysroot/lib/
        cp -rL ${x64-glibc-sysroot}/lib64/.              $out/x64-glibc-sysroot/lib64/ || true
        cp -rL ${x64GlibcDynamicTestBinaries}/bin/.      $out/x64-glibc-dynamic-tests/bin/
        cp -rL ${x64GlibcDynamicCoreutils}/bin/.         $out/x64-glibc-dynamic-coreutils/bin/
      '';

    in {
      # Linux packages (built on x86_64-linux)
      packages.${linuxSystem} = {
        test-binaries = testBinaries;
        guest-bundle = guestBundle;
      };

      # Hydra jobset — ensures ci.zw3rk.com builds guest-bundle and
      # pushes it to cache.zw3rk.com so GHA self-hosted runners can
      # fetch pre-built binaries without needing a Linux builder.
      hydraJobs.${linuxSystem} = {
        guest-bundle = guestBundle;
      };
      hydraJobs.${darwinSystem} = {
        hl = self.packages.${darwinSystem}.default;
      };

      devShells.${darwinSystem} = {
        # Full dev shell with cross-compiled guest test binaries.
        # Requires a configured x86_64-linux remote builder.
        default = darwinPkgs.mkShell {
          name = "hl-dev";

          buildInputs = [
            darwinPkgs.gnumake
            darwinPkgs.binutils  # objcopy for shim.bin
            darwinPkgs.lldb
          ] ++ darwinBuildInputs;

          # GNU objcopy for Mach-O → raw binary conversion (shim.S).
          # The clang wrapper shadows binutils' objcopy with llvm-objcopy,
          # which doesn't handle Mach-O -O binary correctly.
          GNU_OBJCOPY = "${darwinPkgs.binutils}/bin/objcopy";

          # Pre-built guest binaries (dispatched to x86_64-linux builder).
          # NOT added to PATH — they're aarch64-linux ELFs.
          GUEST_TEST_BINARIES = "${testBinaries}";
          GUEST_COREUTILS = "${guestBins.coreutils}";
          GUEST_BUSYBOX   = "${guestBins.busybox}";
          GUEST_STATIC_BINS = "${staticBins}";

          # Dynamic linking tests
          GUEST_SYSROOT          = "${musl-sysroot}";
          GUEST_DYNAMIC_TESTS    = "${dynamicTestBinaries}";
          GUEST_DYNAMIC_COREUTILS = "${dynamicCoreutils}";

          # x86_64-linux-musl test binaries (for rosetta testing)
          GUEST_X64_TEST_BINARIES = "${x64TestBinaries}";
          GUEST_X64_COREUTILS = "${x64GuestBins.coreutils}";
          GUEST_X64_BUSYBOX   = "${x64GuestBins.busybox}";
          GUEST_X64_STATIC_BINS = "${x64StaticBins}";

          # glibc dynamic linking tests (aarch64)
          GUEST_GLIBC_SYSROOT            = "${glibc-sysroot}";
          GUEST_GLIBC_DYNAMIC_TESTS      = "${glibcDynamicTestBinaries}";
          GUEST_GLIBC_DYNAMIC_COREUTILS  = "${glibcDynamicCoreutils}";

          # glibc dynamic linking tests (x86_64)
          GUEST_X64_GLIBC_SYSROOT            = "${x64-glibc-sysroot}";
          GUEST_X64_GLIBC_DYNAMIC_TESTS      = "${x64GlibcDynamicTestBinaries}";
          GUEST_X64_GLIBC_DYNAMIC_COREUTILS  = "${x64GlibcDynamicCoreutils}";

          # Haskell test binary
          GUEST_HASKELL_HELLO = "${haskellHello}";

          # Haskell integration test binaries (pandoc, shellcheck)
          GUEST_HASKELL_BINS = "${haskellBins}";

          shellHook = ''
            echo "hl development environment"
            echo "  make help        — show available targets"
            echo "  make hl          — build the hl executable"
            echo "  make test-all    — run full test suite"
            echo ""
            echo "Guest binaries (aarch64-linux-musl, built on x86_64-linux):"
            echo "  test binaries: $GUEST_TEST_BINARIES/bin/"
            echo "  coreutils:     $GUEST_COREUTILS/bin/"
            echo "  busybox:       $GUEST_BUSYBOX/bin/"
            echo "  static bins:   $GUEST_STATIC_BINS/bin/"
            echo ""
            echo "x86_64-linux-musl guest binaries (for rosetta testing):"
            echo "  x64 tests:    $GUEST_X64_TEST_BINARIES/bin/"
            echo "  x64 coreutils: $GUEST_X64_COREUTILS/bin/"
            echo "  x64 busybox:  $GUEST_X64_BUSYBOX/bin/"
            echo "  x64 static:   $GUEST_X64_STATIC_BINS/bin/"
            echo ""
            echo "Dynamic linking tests (musl):"
            echo "  sysroot:       $GUEST_SYSROOT/lib/"
            echo "  dynamic tests: $GUEST_DYNAMIC_TESTS/bin/"
            echo "  dynamic coreutils: $GUEST_DYNAMIC_COREUTILS/bin/"
            echo ""
            echo "Dynamic linking tests (glibc aarch64):"
            echo "  sysroot:       $GUEST_GLIBC_SYSROOT/lib/"
            echo "  dynamic tests: $GUEST_GLIBC_DYNAMIC_TESTS/bin/"
            echo "  dynamic coreutils: $GUEST_GLIBC_DYNAMIC_COREUTILS/bin/"
            echo ""
            echo "Dynamic linking tests (glibc x86_64):"
            echo "  sysroot:       $GUEST_X64_GLIBC_SYSROOT/lib/"
            echo "  dynamic tests: $GUEST_X64_GLIBC_DYNAMIC_TESTS/bin/"
            echo "  dynamic coreutils: $GUEST_X64_GLIBC_DYNAMIC_COREUTILS/bin/"
            echo ""
            echo "Haskell test binary:"
            echo "  hello-hyper:   $GUEST_HASKELL_HELLO/bin/hello-hyper"
            echo ""
            echo "Haskell integration binaries (native aarch64-linux):"
            echo "  pandoc:        $GUEST_HASKELL_BINS/bin/pandoc"
            echo "  shellcheck:    $GUEST_HASKELL_BINS/bin/shellcheck"
          '';
        };

        # CI-only shell: build tools without cross-compiled test binaries.
        # GitHub Actions macOS runners lack x86_64-linux builders, so the
        # full devShell (which needs cross-compiled GUEST_* binaries) fails.
        ci = darwinPkgs.mkShell {
          name = "hl-ci";
          buildInputs = [
            darwinPkgs.gnumake
            darwinPkgs.binutils
          ] ++ darwinBuildInputs;
          GNU_OBJCOPY = "${darwinPkgs.binutils}/bin/objcopy";
        };
      };

      packages.${darwinSystem}.default = darwinPkgs.stdenv.mkDerivation {
        pname = "hl";
        version = "0.1.0";
        src = ./.;

        nativeBuildInputs = [
          darwinPkgs.gnumake
          darwinPkgs.binutils
          darwinPkgs.xxd
          darwinPkgs.darwin.sigtool  # codesign for nix sandbox
        ] ++ darwinBuildInputs;

        buildInputs = darwinBuildInputs;

        # GNU objcopy for Mach-O → raw binary (clang wrapper shadows with llvm-objcopy)
        GNU_OBJCOPY = "${darwinPkgs.binutils}/bin/objcopy";

        buildPhase = ''
          make hl VERSION="0.1.0+${self.shortRev or "unknown"}"
        '';

        installPhase = ''
          mkdir -p $out/bin $out/share/man/man1
          cp _build/hl $out/bin/hl
          cp hl.1 $out/share/man/man1/hl.1
        '';

        meta = with darwinPkgs.lib; {
          description = "Run aarch64-linux and x86_64-linux ELF binaries on macOS Apple Silicon";
          platforms = [ "aarch64-darwin" ];
          license = licenses.asl20;
        };
      };
    };
}
