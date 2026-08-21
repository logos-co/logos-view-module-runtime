{
  description = "logos-view-module-runtime — shared library for loading and running Logos UI modules";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk = {
      url = "github:logos-co/logos-cpp-sdk";
      inputs.logos-nix.follows = "logos-nix";
    };
    # Master-tracking. This was rev-pinned to c8bab12 on
    # feat/per-client-token-store because logos-qt-host (below) calls
    # TokenManager::forIdentity / isolateIdentity, which were not yet on
    # logos-protocol's master; since logos-plugin-qt's logos-protocol `follows`
    # THIS input, a master-tracking pin would then have built the Qt host
    # runtime against a protocol lacking those symbols.
    #
    # logos-protocol#59 ("per-client token store, the host-services C ABI, and
    # a container shape-check") has since merged, which closes that gap:
    # master (f4407ff) carries forIdentity / isolateIdentity in
    # cpp/token_manager.h and lp_grant_host_services / lp_token_keys in
    # cpp/logos_protocol.h, and its LOGOS_PROTOCOL_VERSION_MINOR reaches the
    # level the cdylib glue's forwarding is guarded on. #59 was SQUASH-merged,
    # so c8bab12 is not an ancestor of master even though every line of it is
    # in master — verify by files, not by `git merge-base --is-ancestor`.
    logos-protocol = {
      url = "github:logos-co/logos-protocol";
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
    # Master-tracking. This was rev-pinned to cc24fa1 (the tip of that repo's
    # feat/b4-qt-host-windows-target) because `logos-qt-host` did not exist on
    # logos-plugin-qt's master, then 8846fc5 — a master-tracking url failed to
    # evaluate with "attribute 'logos-qt-host' missing" — and because that
    # branch was the SUPERSET of the two rival branches carrying the work.
    #
    # logos-plugin-qt#19 ("the Qt host runtime and cdylib-glue generator") has
    # since merged, which closes both gaps: master (9b2c64e) publishes
    # packages.<sys>.logos-qt-host, keyed by forAllTargets so the x86_64-windows
    # pseudo-system resolves as well, and with one master there is no longer a
    # pair of rival branches to keep the downstream closure down to one host.
    # #19 was SQUASH-merged, so cc24fa1 is not an ancestor of master even though
    # its content is — verify by files, not by ancestry. (master also drops the
    # repo's cmake/ directory, whose view-side templates moved to
    # logos-view-module; nothing here ever consumed it.)
    # TEMPORARILY rev-pinned to logos-plugin-qt#23, which adds
    # cpp/logos_plugin_unload.h -- the shared host-side teardown helper ui-host
    # calls below. Re-point at plain master once that merges.
    logos-plugin-qt = {
      url = "github:logos-co/logos-plugin-qt/f30d482d9dc3e1a38d3a9249987d6fb4de4fd9b9";
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
