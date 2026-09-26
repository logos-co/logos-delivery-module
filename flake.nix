{
  description = "Logos Delivery Module";

  # Pull pre-built artifacts (liblogosdelivery, librln, …) from the self-hosted
  # Logos Attic cache. Read-only and public; see infra-ci#263.
  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU=" ];
  };

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
    logos-delivery.url = "git+https://github.com/logos-messaging/logos-delivery?submodules=1";
    # TinyCBOR for the generated binding: nim-ffi's vendored copy, at the rev
    # logos-delivery's nimble.lock pins.
    nim-ffi = {
      url = "github:logos-messaging/nim-ffi/4c1218626bbbf89e19836845b690937cd255c3f0";
      flake = false;
    };
    # The name is load-bearing: the builder resolves the metadata.json
    # dependency of the same name and generates the client from its LIDL.
    libp2p_module.url = "git+https://github.com/logos-co/logos-libp2p-module";
    # The name is load-bearing: the builder resolves each optional_dependencies
    # entry as the input of that name and generates bindings from its LIDL.
    liblogos_rln_module.url = "git+https://github.com/logos-co/logos-rln-modules?ref=main&rev=65697028baffc072e1aeebaec7c7e35e7e12cab1&dir=logos-rln-module";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      # CMakeLists.txt compiles these into the plugin (and the integration tests).
      stageTinycbor = ''
        mkdir -p lib/tinycbor
        cp ${inputs.nim-ffi}/ffi/codegen/templates/cpp/vendor/tinycbor/*.[ch] lib/tinycbor/
        chmod -R u+w lib/tinycbor
      '';
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        externalLibInputs = {
          logosdelivery = {
            input = inputs.logos-delivery;
            packages.default = "liblogosdelivery";
            systems.x86_64-windows = {
              system = "x86_64-linux";
              packages.default = "liblogosdelivery-windows-x86_64";
            };
          };
          # Bundle librln.dylib alongside liblogosdelivery.dylib so the transitive
          # dep resolves at runtime (and during logos-cpp-generator dlopen).
          # Sourced from logos-delivery (not zerokit directly) so we bundle the
          # exact, cargoHash-corrected librln that liblogosdelivery links — zerokit
          # v2.0.2's own rln package has a stale committed cargoHash.
          rln = {
            input = inputs.logos-delivery;
            packages.default = "rln";
            systems.x86_64-windows = {
              system = "x86_64-linux";
              packages.default = "rln-windows-x86_64";
            };
          };
        };
        preConfigure = stageTinycbor;
        tests = {
          dir = ./tests;
          mockCLibs = [ "logosdelivery" ];
          # liblogosdelivery.dylib has a Cargo-baked absolute path to librln.dylib.
          # Rewrite it to @rpath/librln.dylib so the dynamic linker can find it via
          # the lib/ RPATH set on the integration test binary.
          # TODO: remove once logos-module-builder mkLogosModuleTests.nix handles
          # transitive dylib dependency rewriting in its preConfigure (similar to
          # the postInstall rewrite done for the main module build).
          preConfigure = stageTinycbor + ''
            if [ -f lib/liblogosdelivery.dylib ]; then
              OLD_RLN=$(otool -L lib/liblogosdelivery.dylib | awk '/librln/{print $1}')
              if [ -n "$OLD_RLN" ]; then
                install_name_tool -change "$OLD_RLN" "@rpath/librln.dylib" lib/liblogosdelivery.dylib
              fi
            fi
            # Linux: the integration test binary links the staged lib/ libraries
            # by absolute path at build time, but the check phase runs from the
            # build dir where the dynamic linker can't find them. Same class of
            # gap as the darwin rewrite above (see TODO there).
            if [ -f lib/liblogosdelivery.so ]; then
              export LD_LIBRARY_PATH="$(pwd)/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
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
        '';
      };

      # The optional module dependencies (libp2p, the RLN chain), re-exported
      # at the revs this flake locks.
      rlnModule = inputs.liblogos_rln_module;
      lezRlnModule = rlnModule.inputs.liblogos_lez_rln_module;
    in
    module // {
      packages = builtins.mapAttrs (system: pkgs: pkgs // {
        "liblogos_rln_module-lgx" = rlnModule.packages.${system}.lgx;
        "liblogos_lez_rln_module-lgx" = lezRlnModule.packages.${system}.lgx;
      } // (
        # libp2p_module has no build for every system (none for Windows).
        if inputs.libp2p_module.packages ? ${system}
        then { "libp2p_module-lgx" = inputs.libp2p_module.packages.${system}.lgx; }
        else { }
      )) module.packages;
    };
}
