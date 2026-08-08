{ lib, stdenv, libx11, libXext }:
stdenv.mkDerivation {
  pname = "hl-xprobe";
  version = "0.1";
  src = ./.;
  dontConfigure = true;
  buildInputs = [ libx11 libXext ];
  buildPhase = ''
    $CC -O2 -Wall -o hl-xprobe hl-xprobe.c -lX11 -lXext
  '';
  installPhase = ''
    mkdir -p $out/bin
    cp hl-xprobe $out/bin/
  '';
}
