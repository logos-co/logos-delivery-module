{
  description = "Logos Delivery Module";

  # Pull pre-built artifacts (liblogosdelivery, librln, …) from the self-hosted
  # Logos Attic cache. Read-only and public; see infra-ci#263.
  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [ "public:zaDAOkd+E3cQyFm9J7qejQYd9HoLJc1SJvnk68hNWeI=" ];
  };

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
    logos-delivery.url = "git+https://github.com/logos-messaging/logos-delivery?submodules=1&rev=686d3b78084fb567a288378b10297fd31a07de8f";
    # TinyCBOR for the generated binding: nim-ffi's vendored copy, at the rev
    # logos-delivery's nimble.lock pins.
    nim-ffi = {
      url = "github:logos-messaging/nim-ffi/d98f77141de9c95c0c543d191a79aee18e7536a4";
      flake = false;
    };
    # Names are load-bearing: the builder resolves metadata.json's
    # dependencies and optional_dependencies as inputs of those names.
    libp2p_module.url = "git+https://github.com/logos-co/logos-libp2p-module?ref=refs/tags/v1.1.1";
    rln_module = {
      url = "github:logos-co/logos-rln-module/992fab9747aa84a3c72a36176470dccf226130f6";
      inputs.membership_allocation_client_module.url = "github:igor-sirotin/logos-membership-allocation-client-module/82660ca1b8cb19118b2ce1cba72752fc6baa4897";
      inputs.membership_allocation_client_module.inputs.libp2p_module.follows = "libp2p_module";
    };
    # rln_module's own dependencies, at the revs its lock pins (relock with
    # `nix flake update rln_module`). Inputs of this flake only so that it
    # can re-export their `.lgx`s.
    rln_lez_module.follows = "rln_module/rln_lez_module";
    membership_allocation_client_module.follows = "rln_module/membership_allocation_client_module";
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

      # The module dependencies (the RLN chain, optional libp2p), re-exported
      # from their top-level inputs as `<name>-<lgx|install-portable|default>`.
      reexported = [
        "rln_module"
        "rln_lez_module"
        "membership_allocation_client_module"
        "libp2p_module"
      ];
      # Not every module builds for every system (libp2p_module has no
      # Windows build), so an attribute exists only where its input has it.
      reexport = system: name: attr:
        let depPkgs = inputs.${name}.packages.${system} or { }; in
        if depPkgs ? ${attr}
        then { "${name}-${attr}" = depPkgs.${attr}; }
        else { };
    in
    module // {
      packages = builtins.mapAttrs (system: pkgs: pkgs // (
        builtins.foldl' (acc: name:
          acc // (reexport system name "lgx")
          // (reexport system name "install-portable")
          // (reexport system name "default")
        ) { } reexported
      )) module.packages;
    };
}
