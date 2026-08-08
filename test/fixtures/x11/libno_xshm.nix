# nix build -f test/fixtures/x11/libno_xshm.nix --argstr system aarch64-linux -o result-no-xshm
# Or via impure expr that sets system = aarch64-linux.
{ lib, stdenv, libx11, libXext }:
stdenv.mkDerivation {
  pname = "libno-xshm";
  version = "0.1";
  src = ./libno_xshm.c;
  dontUnpack = true;
  buildInputs = [
    libx11
    libXext
  ];
  buildPhase = ''
    $CC -shared -fPIC -O2 -o libno_xshm.so $src -lX11 -lXext
  '';
  installPhase = ''
    mkdir -p $out/lib
    cp libno_xshm.so $out/lib/
  '';
}
