{ pkgs, src, logosSdk, logosLiblogos, logosDelivery }:

pkgs.stdenv.mkDerivation {
  pname = "example-simple";
  version = "0.1";

  inherit src;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsNoGuiHook
    pkgs.gcc
    pkgs.bash
  ];

  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtdeclarative
    pkgs.qt6.qtremoteobjects
    pkgs.gcc
    # Add other dependencies if logos SDK/libs are packaged
  ];

  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_LIBLOGOS_ROOT=${logosLiblogos}"
    "-DLOGOS_DELIVERY_ROOT=${logosDelivery}"
    "-DLOGOS_MESSAGING_MODULE_USE_VENDOR=OFF"
  ];

  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_LIBLOGOS_ROOT = "${logosLiblogos}";
    LOGOS_DELIVERY_ROOT = "${logosDelivery}";
  };

  configurePhase = ''
    # skip default configurePhase
  '';

  # BuildPhase uses CMake for the example
  buildPhase = ''
    mkdir build
    cd build

    cmake ${src} \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$out \
      -GNinja \
      -DLOGOS_CPP_SDK_ROOT=${logosSdk} \
      -DLOGOS_LIBLOGOS_ROOT=${logosLiblogos} \
      -DLOGOS_DELIVERY_ROOT=${logosDelivery} \
      -DLOGOS_MESSAGING_MODULE_USE_VENDOR=OFF

    ls
    cat build.ninja
    # Build the specific example executable
    cmake --build . --target simple_example
  '';

  installPhase = ''
    mkdir -p $out/bin/
    chmod -R 755 $out/bin/
    cp modules/simple_example $out/bin/
  '';
}