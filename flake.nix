{
  description = "hl — aarch64-linux ELF executor on macOS Apple Silicon via Hypervisor.framework";

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

      # ── Dynamic linking test infrastructure ──────────────────────

      # Musl sysroot: dynamic linker + libc.so in /lib/ layout
      musl-sysroot = crossPkgs.runCommand "musl-sysroot" {} ''
        mkdir -p $out/lib
        cp -a ${crossPkgs.musl}/lib/ld-musl-aarch64.so.1 $out/lib/
        cp -a ${crossPkgs.musl}/lib/libc.so $out/lib/
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

    in {
      # Test binaries package (built on x86_64-linux)
      packages.${linuxSystem}.test-binaries = testBinaries;

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
            echo "Dynamic linking tests:"
            echo "  sysroot:       $GUEST_SYSROOT/lib/"
            echo "  dynamic tests: $GUEST_DYNAMIC_TESTS/bin/"
            echo "  dynamic coreutils: $GUEST_DYNAMIC_COREUTILS/bin/"
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
          description = "Run static aarch64-linux ELF binaries on macOS Apple Silicon";
          platforms = [ "aarch64-darwin" ];
          license = licenses.asl20;
        };
      };
    };
}
