{
  description = "logos-view-module-runtime — shared library for loading and running Logos UI modules";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk = {
      url = "github:logos-co/logos-cpp-sdk";
      inputs.logos-nix.follows = "logos-nix";
    };
    # Rev-pinned, not master-tracking: logos-qt-host (below) calls
    # TokenManager::forIdentity / isolateIdentity, which live on
    # logos-protocol's feat/per-client-token-store branch and are NOT on its
    # master. Because logos-plugin-qt's logos-protocol `follows` THIS input,
    # a master-tracking pin here would build the Qt host runtime against a
    # protocol that lacks those symbols. c8bab12 is a fast-forward from
    # master, so nothing on master is given up. Drop the rev once it merges.
    logos-protocol = {
      url = "github:logos-co/logos-protocol/c8bab12834dbf92155b483546875e6078d17c74e";
      inputs.logos-nix.follows = "logos-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    # The Qt HOST RUNTIME this runtime links: LogosAPI (and, through it, the
    # token manager and consumer core). The B1 split moved it out of
    # logos-qt-sdk into logos-plugin-qt, which exports it as
    # packages.<sys>.logos-qt-host with the CMake target
    # logos-qt-host::logos_qt_host.
    #
    # logos-qt-sdk is deliberately NOT an input any more: the host runtime was
    # the only thing this repo ever took from it. `logos_api.h` is the single
    # qt-sdk-provided header anything here includes — every other non-Qt header
    # (token_manager.h, logos_api_client.h, module_proxy.h, remote_transport.h,
    # logos_instance.h, logos_mode.h, logos_types.h, logos_json_convert.h,
    # logos_call_error.h, logos_object.h, logos_provider_interface.h) comes from
    # logos-protocol, which logos-qt-host links PUBLIC. Nothing here touches the
    # surface that stays behind in qt-sdk: no logos_ui_plugin_context.h (that is
    # for ui_qml module backends, not for the host that loads them), no
    # logos_qt_lp_bridge.h / logos_qt_wire.h, no logos-qt-generator.
    #
    # Rev-pinned for the same reason logos-protocol is: `logos-qt-host` does
    # not exist on logos-plugin-qt's master (8846fc5) — a master-tracking url
    # fails to evaluate with "attribute 'logos-qt-host' missing". cc24fa1 is
    # the tip of that repo's feat/b4-qt-host-windows-target, already rebased
    # onto its master. It is the SUPERSET of the two branches carrying this
    # work; the sibling feat/b4-qt-host-windows-target-8ccb1fc (989f6ae) omits
    # commits that logos-module-builder pins, so pinning the superset here is
    # what keeps one logos-qt-host in the downstream closure instead of two.
    # Drop the rev once it merges.
    logos-plugin-qt = {
      url = "github:logos-co/logos-plugin-qt/cc24fa1c0c43b2d96c1dc165ee545a0321318b59";
      inputs.logos-nix.follows = "logos-nix";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.logos-protocol.follows = "logos-protocol";
    };
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-plugin-qt }:
    let
      # Adds the "x86_64-windows" pseudo-system. A cross derivation's `system`
      # attr is its BUILD platform, so these evaluate anywhere and realise on
      # x86_64-linux.
      forAllSystems = f: logos-nix.lib.forAllTargets ({ system, pkgs }: f {
        inherit system pkgs;
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosQtHost = logos-plugin-qt.packages.${system}.logos-qt-host;
        logosProtocol = logos-protocol.packages.${system}.default;
      });
    in
    {
      packages = forAllSystems ({ pkgs, logosSdk, logosQtHost, logosProtocol, ... }: {
        default = import ./nix/default.nix { inherit pkgs logosSdk logosQtHost logosProtocol; };
        tests = import ./nix/test.nix { inherit pkgs logosSdk logosQtHost logosProtocol; };
      });

      checks = forAllSystems ({ pkgs, logosSdk, logosQtHost, logosProtocol, ... }: {
        default = import ./nix/test.nix { inherit pkgs logosSdk logosQtHost logosProtocol; };
      });

      devShells = forAllSystems ({ pkgs, logosSdk, logosQtHost, logosProtocol, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.qt6.qtdeclarative
          ];
          shellHook = ''
            export LOGOS_CPP_SDK_ROOT="${logosSdk}"
            export LOGOS_QT_HOST_ROOT="${logosQtHost}"
            export LOGOS_PROTOCOL_ROOT="${logosProtocol}"
            echo "logos-view-module-runtime dev shell"
          '';
        };
      });
    };
}
