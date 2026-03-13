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
      aarch64LinuxPkgs = nixpkgs.legacyPackages.aarch64-linux;

      # Cross-compilation package sets
      crossPkgs      = linuxPkgs.pkgsCross.aarch64-multiplatform-musl;
      x64CrossPkgs   = linuxPkgs.pkgsCross.musl64;
      glibcCrossPkgs = linuxPkgs.pkgsCross.aarch64-multiplatform;
      x64GlibcPkgs   = linuxPkgs;  # native x86_64-linux with glibc

      # Darwin SDK for Hypervisor.framework
      darwinBuildInputs = [
        darwinPkgs.apple-sdk_15
        (darwinPkgs.darwinMinVersionHook "15.0")
      ];

      # Import modular derivation sets
      guest = import ./nix/guest-bins.nix {
        inherit linuxPkgs crossPkgs x64CrossPkgs glibcCrossPkgs x64GlibcPkgs;
      };

      haskell = import ./nix/haskell.nix {
        lib = linuxPkgs.lib;
        inherit linuxPkgs crossPkgs x64CrossPkgs aarch64LinuxPkgs;
      };

    in {

      # ── Packages ────────────────────────────────────────────────────

      packages.${linuxSystem} = {
        # Bundles (CI artifact aggregates — used by ci.yml)
        guest-bundle       = guest.guestBundle;
        haskell-x64-bundle = haskell.x64HaskellBundle;
        # Individual guest components
        test-binaries      = guest.testBinaries;
        x64-test-binaries  = guest.x64TestBinaries;
        coreutils          = guest.coreutils;
        x64-coreutils      = guest.x64Coreutils;
        busybox            = guest.busybox;
        x64-busybox        = guest.x64Busybox;
        static-bins        = guest.staticBins;
        x64-static-bins    = guest.x64StaticBins;
        musl-sysroot       = guest.muslSysroot;
        x64-musl-sysroot   = guest.x64MuslSysroot;
        glibc-sysroot      = guest.glibcSysroot;
        x64-glibc-sysroot  = guest.x64GlibcSysroot;
        # Haskell
        x64-haskell-hello  = haskell.x64HaskellHello;
        x64-haskell-bins   = haskell.x64HaskellBins;
      };

      packages.aarch64-linux = {
        haskell-bundle     = haskell.haskellBundle;
        haskell-bins       = haskell.haskellBins;
        haskell-hello      = haskell.haskellHello;
      };

      packages.${darwinSystem}.default = darwinPkgs.stdenv.mkDerivation {
        pname = "hl";
        version = "0.2.3";
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

        # Prevent nix from stripping the binary (removes codesign)
        dontStrip = true;

        buildPhase = ''
          make hl VERSION="$version+${self.shortRev or "unknown"}"
        '';

        installPhase = ''
          mkdir -p $out/bin $out/share/man/man1
          cp _build/hl $out/bin/hl
          cp hl.1 $out/share/man/man1/hl.1
        '';

        # Re-sign after install to ensure Hypervisor entitlement survives
        postFixup = ''
          codesign --entitlements entitlements.plist -f -s "-" $out/bin/hl
        '';

        meta = with darwinPkgs.lib; {
          description = "Run aarch64-linux and x86_64-linux ELF binaries on macOS Apple Silicon";
          mainProgram = "hl";
          platforms = [ "aarch64-darwin" ];
          license = licenses.asl20;
        };
      };

      # ── Hydra Jobs ──────────────────────────────────────────────────
      # Every package is a Hydra job.  When a single component changes
      # (e.g., bumping jq in static-bins), only that component rebuilds.
      hydraJobs.${linuxSystem}  = self.packages.${linuxSystem};
      hydraJobs.aarch64-linux   = self.packages.aarch64-linux;
      hydraJobs.${darwinSystem} = {
        hl = self.packages.${darwinSystem}.default;
      };

      # ── Dev Shells ──────────────────────────────────────────────────

      devShells.${darwinSystem} = {
        # Full dev shell with cross-compiled guest test binaries.
        # Requires a configured x86_64-linux remote builder.
        default = darwinPkgs.mkShell {
          name = "hl-dev";

          buildInputs = [
            darwinPkgs.gnumake
            darwinPkgs.binutils   # objcopy for shim.bin
            darwinPkgs.lldb
            darwinPkgs.shellcheck  # shell script linting
          ] ++ darwinBuildInputs;

          # GNU objcopy for Mach-O → raw binary conversion (shim.S).
          # The clang wrapper shadows binutils' objcopy with llvm-objcopy,
          # which doesn't handle Mach-O -O binary correctly.
          GNU_OBJCOPY = "${darwinPkgs.binutils}/bin/objcopy";

          # Lima VM runner (for validation against real Linux kernel).
          LIMACTL = "${darwinPkgs.lima}/bin/limactl";

          # aarch64-linux-musl guest binaries
          GUEST_TEST_BINARIES     = "${guest.testBinaries}";
          GUEST_COREUTILS         = "${guest.coreutils}";
          GUEST_BUSYBOX           = "${guest.busybox}";
          GUEST_STATIC_BINS       = "${guest.staticBins}";
          GUEST_SYSROOT           = "${guest.muslSysroot}";
          GUEST_DYNAMIC_TESTS     = "${guest.dynamicTestBinaries}";
          GUEST_DYNAMIC_COREUTILS = "${guest.dynamicCoreutils}";

          # x86_64-linux-musl guest binaries
          GUEST_X64_TEST_BINARIES          = "${guest.x64TestBinaries}";
          GUEST_X64_COREUTILS              = "${guest.x64Coreutils}";
          GUEST_X64_BUSYBOX                = "${guest.x64Busybox}";
          GUEST_X64_STATIC_BINS            = "${guest.x64StaticBins}";
          GUEST_X64_MUSL_SYSROOT           = "${guest.x64MuslSysroot}";
          GUEST_X64_MUSL_DYNAMIC_TESTS     = "${guest.x64MuslDynamicTestBinaries}";
          GUEST_X64_MUSL_DYNAMIC_COREUTILS = "${guest.x64MuslDynamicCoreutils}";

          # x86_64 Haskell hello (musl, dynamically linked)
          GUEST_X64_HASKELL_HELLO = "${haskell.x64HaskellHello}";

          # glibc dynamic linking tests (aarch64)
          GUEST_GLIBC_SYSROOT            = "${guest.glibcSysroot}";
          GUEST_GLIBC_DYNAMIC_TESTS      = "${guest.glibcDynamicTestBinaries}";
          GUEST_GLIBC_DYNAMIC_COREUTILS  = "${guest.glibcDynamicCoreutils}";

          # glibc dynamic linking tests (x86_64)
          GUEST_X64_GLIBC_SYSROOT            = "${guest.x64GlibcSysroot}";
          GUEST_X64_GLIBC_DYNAMIC_TESTS      = "${guest.x64GlibcDynamicTestBinaries}";
          GUEST_X64_GLIBC_DYNAMIC_COREUTILS  = "${guest.x64GlibcDynamicCoreutils}";

          # Haskell test binaries
          GUEST_HASKELL_HELLO    = "${haskell.haskellHello}";
          GUEST_HASKELL_BINS     = "${haskell.haskellBins}";
          GUEST_X64_HASKELL_BINS = "${haskell.x64HaskellBins}";

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
            echo "x86_64 dynamic linking tests (musl):"
            echo "  sysroot:       $GUEST_X64_MUSL_SYSROOT/lib/"
            echo "  dynamic tests: $GUEST_X64_MUSL_DYNAMIC_TESTS/bin/"
            echo "  dynamic coreutils: $GUEST_X64_MUSL_DYNAMIC_COREUTILS/bin/"
            echo ""
            echo "x86_64 Haskell test binary:"
            echo "  hello-hyper:   $GUEST_X64_HASKELL_HELLO/bin/hello-hyper"
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
            echo ""
            echo "Haskell integration binaries (native x86_64-linux):"
            echo "  pandoc:        $GUEST_X64_HASKELL_BINS/bin/pandoc"
            echo "  shellcheck:    $GUEST_X64_HASKELL_BINS/bin/shellcheck"
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
            darwinPkgs.xxd        # needed by shim_blob.h generation
            darwinPkgs.shellcheck  # shell script linting
          ] ++ darwinBuildInputs;
          GNU_OBJCOPY = "${darwinPkgs.binutils}/bin/objcopy";
        };
      };
    };
}
