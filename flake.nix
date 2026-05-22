{
  description = "Logos Delivery Module";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
    logos-delivery.url = "git+https://github.com/logos-messaging/logos-delivery?submodules=1";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      externalLibInputs = {
        logosdelivery = {
          input = inputs.logos-delivery;
          packages.default = "liblogosdelivery";
        };
        # Bundle librln.dylib alongside liblogosdelivery.dylib so the transitive
        # dep resolves at runtime (and during logos-cpp-generator dlopen).
        # Sourced from logos-delivery (not zerokit directly) so we bundle the
        # exact, cargoHash-corrected librln that liblogosdelivery links — zerokit
        # v2.0.2's own rln package has a stale committed cargoHash.
        rln = {
          input = inputs.logos-delivery;
          packages.default = "rln";
        };
      };
      tests = {
        dir = ./tests;
        mockCLibs = [ "logosdelivery" ];
        # liblogosdelivery.dylib has a Cargo-baked absolute path to librln.dylib.
        # Rewrite it to @rpath/librln.dylib so the dynamic linker can find it via
        # the lib/ RPATH set on the integration test binary.
        # TODO: remove once logos-module-builder mkLogosModuleTests.nix handles
        # transitive dylib dependency rewriting in its preConfigure (similar to
        # the postInstall rewrite done for the main module build).
        preConfigure = ''
          if [ -f lib/liblogosdelivery.dylib ]; then
            OLD_RLN=$(otool -L lib/liblogosdelivery.dylib | awk '/librln/{print $1}')
            if [ -n "$OLD_RLN" ]; then
              install_name_tool -change "$OLD_RLN" "@rpath/librln.dylib" lib/liblogosdelivery.dylib
            fi
          fi
        '';
      };
      # Bundle runtime libraries alongside the plugin.
      postInstall = ''
        # liblogosdelivery.dylib has a sandbox-baked absolute path for librln.dylib
        # (Cargo bakes the build-time path as the install name). Rewrite it to
        # @rpath/librln.dylib so the dynamic linker finds it via @loader_path.
        if [ -f "$out/lib/liblogosdelivery.dylib" ]; then
          OLD_RLN=$(otool -L "$out/lib/liblogosdelivery.dylib" | awk '/librln/{print $1}')
          if [ -n "$OLD_RLN" ]; then
            echo "Fixing librln rpath in liblogosdelivery.dylib: $OLD_RLN -> @rpath/librln.dylib"
            install_name_tool -change "$OLD_RLN" "@rpath/librln.dylib" \
              "$out/lib/liblogosdelivery.dylib"
          fi

          # Add @loader_path/. as an rpath so that Nim's runtime dlopen("libpq.dylib")
          # finds the bundled libpq in the same directory as liblogosdelivery.dylib.
          if ! otool -l "$out/lib/liblogosdelivery.dylib" | awk '
            $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
            in_rpath && $1 == "path" { print $2; in_rpath = 0 }
          ' | grep -Fxq "@loader_path/."; then
            install_name_tool -add_rpath "@loader_path/." \
              "$out/lib/liblogosdelivery.dylib"
          fi
        fi
        
        # Use pkg-config to locate the exact libpq from the build environment
        LIBPQ_LIBDIR=$(pkg-config --variable=libdir libpq 2>/dev/null || true)
        if [ -n "$LIBPQ_LIBDIR" ] && [ -d "$LIBPQ_LIBDIR" ]; then
          for f in "$LIBPQ_LIBDIR"/libpq.*; do
            [ -f "$f" ] && cp -L "$f" $out/lib/ 2>/dev/null || true
          done
        fi

        # libpq is loaded at runtime via dlopen/dlsym (not a linked dependency),
        # so install_name_tool has no effect on macOS — otool -L won't show libpq.
        # On Linux, dlopen with a bare name searches the calling library's DT_RUNPATH,
        # so setting $ORIGIN makes libpq.so discoverable from the same directory.
        if [ -f "$out/lib/liblogosdelivery.so" ]; then
          echo "Fixing rpath in liblogosdelivery.so: adding \$ORIGIN for dlopen libpq resolution"
          chmod u+w "$out/lib/liblogosdelivery.so"
          patchelf --set-rpath '$ORIGIN' "$out/lib/liblogosdelivery.so"
        fi
      '';
    };
}
