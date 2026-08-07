{
  description = "logos-view-module-runtime — shared library for loading and running Logos UI modules";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk = {
      url = "github:logos-co/logos-cpp-sdk";
      inputs.logos-nix.follows = "logos-nix";
    };
    logos-protocol = {
      url = "github:logos-co/logos-protocol";
      inputs.logos-nix.follows = "logos-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    logos-qt-sdk = {
      url = "github:logos-co/logos-qt-sdk";
      inputs.logos-nix.follows = "logos-nix";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.logos-protocol.follows = "logos-protocol";
      inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    };
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-qt-sdk }:
    let
      # Adds the "x86_64-windows" pseudo-system. A cross derivation's `system`
      # attr is its BUILD platform, so these evaluate anywhere and realise on
      # x86_64-linux.
      forAllSystems = f: logos-nix.lib.forAllTargets ({ system, pkgs }: f {
        inherit system pkgs;
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosQtSdk = logos-qt-sdk.packages.${system}.default;
        logosProtocol = logos-protocol.packages.${system}.default;
      });
    in
    {
      packages = forAllSystems ({ pkgs, logosSdk, logosQtSdk, logosProtocol, ... }: {
        default = import ./nix/default.nix { inherit pkgs logosSdk logosQtSdk logosProtocol; };
        tests = import ./nix/test.nix { inherit pkgs logosSdk logosQtSdk logosProtocol; };
      });

      checks = forAllSystems ({ pkgs, logosSdk, logosQtSdk, logosProtocol, ... }: {
        default = import ./nix/test.nix { inherit pkgs logosSdk logosQtSdk logosProtocol; };
      });

      devShells = forAllSystems ({ pkgs, logosSdk, logosQtSdk, logosProtocol, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.qt6.qtdeclarative
          ];
          shellHook = ''
            export LOGOS_CPP_SDK_ROOT="${logosSdk}"
            export LOGOS_QT_SDK_ROOT="${logosQtSdk}"
            export LOGOS_PROTOCOL_ROOT="${logosProtocol}"
            echo "logos-view-module-runtime dev shell"
          '';
        };
      });
    };
}
