{
  description = "Logos Delivery Module";

  inputs = {
    # Follow the same nixpkgs as logos-liblogos to ensure compatibility
    nixpkgs.follows = "logos-liblogos/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk?ref=feat/logos-result";
    logos-liblogos.url = "github:logos-co/logos-liblogos";
    logos-delivery.url = "git+https://github.com/logos-messaging/logos-delivery?ref=add-debug-api&submodules=1";
  };

  outputs = { self, nixpkgs, logos-cpp-sdk, logos-liblogos, logos-delivery }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosLiblogos = logos-liblogos.packages.${system}.default;
        logosDelivery = (logos-delivery.packages.${system}.liblogosdelivery).overrideAttrs (old: {
          NIMFLAGS = (old.NIMFLAGS or "") + " -d:postgres  -d:nimDebugDlOpen -d:chronicles_colors:none  ";
        });
      });
    in
    {
      packages = forAllSystems ({ pkgs, logosSdk, logosLiblogos, logosDelivery }:
        let
          common = import ./nix/default.nix { inherit pkgs logosSdk logosLiblogos logosDelivery; };
          src = ./.;
          lib = import ./nix/lib.nix { inherit pkgs common src logosDelivery; };
          include = import ./nix/header.nix { inherit pkgs common src lib logosSdk logosDelivery; };
          simple = import ./nix/examples.nix { inherit pkgs src logosSdk logosLiblogos logosDelivery; };
          combined = pkgs.symlinkJoin {
            name = "logos-delivery-module";
            paths = [ lib include simple ];
          };
        in
        {
          lib = lib;
          include = include;
          default = combined;

          # Add new output
          simple = simple;
        }
      );

      devShells = forAllSystems ({ pkgs, logosSdk, logosLiblogos, logosDelivery }: {
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
            export LOGOS_DELIVERY_ROOT="${logosDelivery}"
            echo "Logos Delivery Module development environment"
            echo "LOGOS_CPP_SDK_ROOT: $LOGOS_CPP_SDK_ROOT"
            echo "LOGOS_LIBLOGOS_ROOT: $LOGOS_LIBLOGOS_ROOT"
            echo "LOGOS_DELIVERY_ROOT: $LOGOS_DELIVERY_ROOT"
          '';
        };
      });
    };
}
