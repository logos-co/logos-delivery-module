# Generates headers from the messaging module plugin using logos-cpp-generator
{ pkgs, common, src, lib, logosSdk, logosDelivery }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-headers";
  version = common.version;
  
  inherit src;
  inherit (common) meta;
  
  # We need the generator and the built plugin
  nativeBuildInputs = [ logosSdk ];
  buildInputs = [ pkgs.qt6.qtbase pkgs.qt6.qtremoteobjects ];
  
  # No configure phase needed
  dontConfigure = true;
  dontWrapQtApps = true;
  
  buildPhase = ''
    runHook preBuild
    
    # Create output directory for generated headers
    mkdir -p ./generated_headers
    
    # Determine platform-specific library extension
    if [ -f "${lib}/lib/delivery_module_plugin.dylib" ]; then
      PLUGIN_FILE="${lib}/lib/delivery_module_plugin.dylib"
    elif [ -f "${lib}/lib/delivery_module_plugin.so" ]; then
      PLUGIN_FILE="${lib}/lib/delivery_module_plugin.so"
    else
      echo "Error: No delivery_module_plugin library file found"
      exit 1
    fi
    
    # Set library path so the plugin can find liblogosdelivery when loaded
    if [ "$(uname -s)" = "Darwin" ]; then
      export DYLD_LIBRARY_PATH="${lib}/lib:''${DYLD_LIBRARY_PATH:-}"
    else
      export LD_LIBRARY_PATH="${lib}/lib:${pkgs.qt6.qtbase}/lib:${pkgs.qt6.qtremoteobjects}/lib:''${LD_LIBRARY_PATH:-}"
    fi
    
    # Run logos-cpp-generator on the built plugin with --module-only flag
    echo "Running logos-cpp-generator on $PLUGIN_FILE"
    echo "Library path: ${lib}/lib"
    ls -la "${lib}/lib"
    logos-cpp-generator "$PLUGIN_FILE" --output-dir ./generated_headers --module-only || {
      echo "Warning: logos-cpp-generator failed, this may be expected if the module has no public API"
      # Create a marker file to indicate attempt was made
      touch ./generated_headers/.no-api
    }
    
    runHook postBuild
  '';
  
  installPhase = ''
    runHook preInstall
    
    # Install generated headers
    mkdir -p $out/include
    
    # Copy all generated files to include/ if they exist
    if [ -d ./generated_headers ] && [ "$(ls -A ./generated_headers 2>/dev/null)" ]; then
      echo "Copying generated headers..."
      ls -la ./generated_headers
      cp -r ./generated_headers/. $out/include/
    else
      echo "Warning: No generated headers found, creating empty include directory"
      # Create a placeholder file to indicate headers should be generated from metadata
      echo "# Generated headers from metadata.json" > $out/include/.generated
    fi

    # Copy header from logos-delivery
    echo "Copying header from logos-delivery..."
    if [ -d "${logosDelivery}/include" ]; then
      echo "Found include directory in logos-delivery"
      cp -r "${logosDelivery}/include"/. $out/include/
    else
      echo "Warning: No include directory found in logos-delivery"
    fi

    echo "Copied include files:"
    ls -la $out/include/

    runHook postInstall
  '';
}
