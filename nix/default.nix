# Common build configuration shared across all packages
{ pkgs, logosSdk, logosLiblogos, logosMessagingNim }:

{
  pname = "logos-delivery-module";
  version = "1.0.0";
  
  # Common native build inputs
  nativeBuildInputs = [ 
    pkgs.cmake 
    pkgs.ninja 
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsNoGuiHook
  ];
  
  # Common runtime dependencies
  buildInputs = [ 
    pkgs.qt6.qtbase 
    pkgs.qt6.qtremoteobjects 
  ];
  
  # Common CMake flags
  cmakeFlags = [ 
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_LIBLOGOS_ROOT=${logosLiblogos}"
    "-DLOGOS_MESSAGING_NIM_ROOT=${logosMessagingNim}"
    "-DLOGOS_MESSAGING_MODULE_USE_VENDOR=OFF"
  ];
  
  # Environment variables
  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_LIBLOGOS_ROOT = "${logosLiblogos}";
    LOGOS_MESSAGING_NIM_ROOT = "${logosMessagingNim}";
  };
  
  # Metadata
  meta = with pkgs.lib; {
    description = "Logos Delivery Module - Provides Logos Delivery communication capabilities";
    platforms = platforms.unix;
  };
}
