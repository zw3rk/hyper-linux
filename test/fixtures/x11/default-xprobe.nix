let
  lock = builtins.fromJSON (builtins.readFile ../../../flake.lock);
  np = lock.nodes.nixpkgs.locked;
  pkgs = import (builtins.fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/${np.rev}.tar.gz";
    sha256 = np.narHash;
  }) { system = "aarch64-linux"; };
in
pkgs.stdenv.mkDerivation {
  pname = "hl-xprobe";
  version = "0.1";
  src = pkgs.lib.cleanSourceWith {
    src = ./.;
    filter = path: type:
      baseNameOf path == "hl-xprobe.c";
  };
  buildInputs = [ pkgs.libx11 pkgs.xorg.libXext or pkgs.libxext ];
  buildPhase = ''
    $CC -O2 -Wall -o hl-xprobe hl-xprobe.c -lX11 -lXext
  '';
  installPhase = ''
    mkdir -p $out/bin
    cp hl-xprobe $out/bin/
  '';
}
