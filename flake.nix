{
  description = "logos-view-module-runtime — shared library for loading and running Logos UI modules";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk = {
      url = "github:logos-co/logos-cpp-sdk";
      inputs.logos-nix.follows = "logos-nix";
    };
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
      });
    in
    {
      packages = forAllSystems ({ pkgs, logosSdk, ... }: {
        default = import ./nix/default.nix { inherit pkgs logosSdk; };
        tests = import ./nix/test.nix { inherit pkgs logosSdk; };
      });

      checks = forAllSystems ({ pkgs, logosSdk, ... }: {
        default = import ./nix/test.nix { inherit pkgs logosSdk; };
      });

      devShells = forAllSystems ({ pkgs, logosSdk, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.qt6.qtdeclarative
          ];
          shellHook = ''
            export LOGOS_CPP_SDK_ROOT="${logosSdk}"
            echo "logos-view-module-runtime dev shell"
          '';
        };
      });
    };
}
