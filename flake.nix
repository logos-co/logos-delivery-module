{
  description = "Logos Messaging Module";

  inputs = {
    # Follow the same nixpkgs as logos-liblogos to ensure compatibility
    nixpkgs.follows = "logos-liblogos/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk?ref=feat/logos-result";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-messaging.url =  "git+https://github.com/logos-messaging/logos-messaging-nim?ref=feat-lmapi-lib&rev=417c55786888a15f12288586997faef5c97cb403&submodules=1";
  };

  outputs = { self, nixpkgs, logos-cpp-sdk, logos-liblogos, logos-messaging }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosLiblogos = logos-liblogos.packages.${system}.default;
        logosMessagingNim = logos-messaging.packages.${system}.liblogosdelivery;
      });
    in
    {
      packages = forAllSystems ({ pkgs, logosSdk, logosLiblogos, logosMessagingNim }:
        let
          # Common configuration
          common = import ./nix/default.nix { inherit pkgs logosSdk logosLiblogos logosMessagingNim; };
          src = ./.;
          
          # Library package (plugin + libcodex)
          lib = import ./nix/lib.nix { inherit pkgs common src logosMessagingNim; };
          
          # Include package (generated headers from plugin)
          include = import ./nix/include.nix { inherit pkgs common src lib logosSdk logosMessagingNim; };
          
          # Combined package
          combined = pkgs.symlinkJoin {
            name = "logos-delivery-module";
            paths = [ lib include ];
          };
        in
        {
          # Individual outputs
          lib = lib;
          include = include;
          
          # Default package (combined)
          default = combined;
        }
      );

      devShells = forAllSystems ({ pkgs, logosSdk, logosLiblogos, logosMessagingNim }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
          ];
          
          shellHook = ''
            export LOGOS_CPP_SDK_ROOT="${logosSdk}"
            export LOGOS_LIBLOGOS_ROOT="${logosLiblogos}"
            export LOGOS_MESSAGING_NIM_ROOT="${logosMessagingNim}"
            echo "Logos Delivery Module development environment"
            echo "LOGOS_CPP_SDK_ROOT: $LOGOS_CPP_SDK_ROOT"
            echo "LOGOS_LIBLOGOS_ROOT: $LOGOS_LIBLOGOS_ROOT"
            echo "LOGOS_MESSAGING_NIM_ROOT: $LOGOS_MESSAGING_NIM_ROOT"
          '';
        };
      });
    };
}
