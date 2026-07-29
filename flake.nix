{
  description = "Logos Delivery Module";

  # Pull pre-built artifacts (liblogosdelivery, librln, …) from the self-hosted
  # Logos Attic cache. Read-only and public; see infra-ci#263.
  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU=" ];
  };

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.5";
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

        # librln.dylib is copied out of zerokit's output, so everything it loads
        # by absolute store path is a dependency of zerokit and not of this
        # module. A module travels to an app inside an LGX archive, which nix
        # cannot scan for store paths, so nothing installs those alongside the
        # module and the plugin fails to dlopen wherever they do not already
        # exist. Bundle them next to librln and load them through @loader_path,
        # the way librln and libpq already travel with the module. Transitively:
        # the libiconv librln loads re-exports libcharset from the same path.
        pending="$out/lib/librln.dylib"
        while [ -n "$pending" ]; do
          next=""
          for macho in $pending; do
            [ -f "$macho" ] || continue
            chmod u+w "$macho"
            for dep in $(otool -l "$macho" | awk '
              $1 == "cmd" { load = ($2 ~ /^LC_(LOAD_DYLIB|LOAD_WEAK_DYLIB|REEXPORT_DYLIB)$/) }
              load && $1 == "name" && $2 ~ "^/nix/store/" { print $2 }
            '); do
              name=$(basename "$dep")
              if [ ! -f "$out/lib/$name" ]; then
                echo "Bundling $dep as @loader_path/$name"
                cp -L "$dep" "$out/lib/$name"
                chmod u+w "$out/lib/$name"
                install_name_tool -id "@loader_path/$name" "$out/lib/$name"
                next="$next $out/lib/$name"
              fi
              install_name_tool -change "$dep" "@loader_path/$name" "$macho"
            done
          done
          pending="$next"
        done

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
