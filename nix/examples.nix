{ pkgs ? import <nixpkgs> {} }:

pkgs.stdenv.mkDerivation {
  pname = "example-simple";
  version = "0.1";

  # Source directory of the project
  src = ./.;

  # Dependencies
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.gcc
    pkgs.make
  ];

  # Build the simple.cpp example
  buildPhase = ''
    mkdir -p build
    g++ -std=c++17 \
        -I${pkgs.qt6.qtbase.dev}/include/QtCore \
        -I${src}/delivery_interface \
        ${src}/examples/simple.cpp \
        -L${pkgs.qt6.qtbase.out}/lib -lQt6Core \
        -o build/simple
  '';

  # Install binary
  installPhase = ''
    mkdir -p $out/bin
    cp build/simple $out/bin/
  '';
}