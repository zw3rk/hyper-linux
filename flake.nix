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

          # All C test programs (static musl)
          for f in *.c; do
            name="''${f%.c}"
            $CC -static -O2 -o "$name" "$f"
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

    in {
      # Test binaries package (built on x86_64-linux)
      packages.${linuxSystem}.test-binaries = testBinaries;

      devShells.${darwinSystem}.default = darwinPkgs.mkShell {
        name = "hl-dev";

        buildInputs = [
          darwinPkgs.gnumake
          darwinPkgs.binutils  # objcopy for shim.bin
          darwinPkgs.lldb
        ] ++ darwinBuildInputs;

        # Pre-built guest binaries (dispatched to x86_64-linux builder).
        # NOT added to PATH — they're aarch64-linux ELFs.
        GUEST_TEST_BINARIES = "${testBinaries}";
        GUEST_COREUTILS = "${guestBins.coreutils}";
        GUEST_BUSYBOX   = "${guestBins.busybox}";

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
        '';
      };

      packages.${darwinSystem}.default = darwinPkgs.stdenv.mkDerivation {
        pname = "hl";
        version = "0.1.0";
        src = ./.;

        nativeBuildInputs = [
          darwinPkgs.gnumake
          darwinPkgs.binutils
        ] ++ darwinBuildInputs;

        buildInputs = darwinBuildInputs;

        buildPhase = ''
          make hl
        '';

        installPhase = ''
          mkdir -p $out/bin
          cp _build/hl $out/bin/hl
        '';

        meta = with darwinPkgs.lib; {
          description = "Run static aarch64-linux ELF binaries on macOS Apple Silicon";
          platforms = [ "aarch64-darwin" ];
          license = licenses.asl20;
        };
      };
    };
}
