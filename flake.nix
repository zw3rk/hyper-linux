{
  description = "hl — aarch64-linux ELF executor on macOS Apple Silicon via Hypervisor.framework";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "aarch64-darwin";
      pkgs = nixpkgs.legacyPackages.${system};

      # Darwin SDK for Hypervisor.framework
      darwinBuildInputs = [
        pkgs.apple-sdk_15
        (pkgs.darwinMinVersionHook "15.0")
      ];

      # Cross-compiler for building aarch64-linux test binaries
      crossCC = pkgs.pkgsCross.aarch64-multiplatform-musl.stdenv.cc;

    in {
      devShells.${system}.default = pkgs.mkShell {
        name = "hl-dev";

        buildInputs = [
          pkgs.gnumake
          pkgs.binutils  # objcopy for shim.bin
          pkgs.lldb
          crossCC        # aarch64-unknown-linux-musl-{as,ld,gcc}
        ] ++ darwinBuildInputs;

        shellHook = ''
          echo "hl development environment"
          echo "  make help        — show available targets"
          echo "  make hl          — build the hl executable"
          echo "  make test-hello  — build and run assembly hello world"
        '';
      };

      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "hl";
        version = "0.1.0";
        src = ./.;

        nativeBuildInputs = [
          pkgs.gnumake
          pkgs.binutils
        ] ++ darwinBuildInputs;

        buildInputs = darwinBuildInputs;

        buildPhase = ''
          make hl
        '';

        installPhase = ''
          mkdir -p $out/bin
          cp _build/hl $out/bin/hl
        '';

        meta = with pkgs.lib; {
          description = "Run static aarch64-linux ELF binaries on macOS Apple Silicon";
          platforms = [ "aarch64-darwin" ];
          license = licenses.asl20;
        };
      };
    };
}
