{ pkgs, logosSdk, logosQtSdk, logosProtocol }:

pkgs.stdenv.mkDerivation {
  pname = "logos-view-module-runtime";
  version = "1.0.0";

  src = ../.;

  # Required wherever the Qt wrapper hooks are absent (see below).
  dontWrapQtApps = true;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
  ]
  # wrapQtAppsHook fails to EVALUATE for a mingw host, and wrap-qt-apps-hook.sh
  # would skip a PE anyway (`isELF || isMachO || continue`). Gating it off is
  # only half the fix -- qtbase's setup hook hard-errors unless
  # dontWrapQtApps is also set below.
  ++ pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows) pkgs.qt6.wrapQtAppsHook;

  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    pkgs.qt6.qtdeclarative
    logosSdk
    logosQtSdk
    logosProtocol
  ];

  dontStrip = true;

  preConfigure = ''
    export MACOSX_DEPLOYMENT_TARGET=12.0

    # Point CMake at the SDK store path directly. CMakeLists.txt
    # uses `find_package(logos-cpp-sdk CONFIG PATHS
    # $LOGOS_CPP_SDK_ROOT/lib/cmake/logos-cpp-sdk)`, which carries
    # include dirs + the link interface (OpenSSL, Boost, nlohmann)
    # via the imported target — no need to stage a vendored copy.
    cmakeFlagsArray+=("-DLOGOS_CPP_SDK_ROOT=${logosSdk}")
    cmakeFlagsArray+=("-DLOGOS_QT_SDK_ROOT=${logosQtSdk}")
    cmakeFlagsArray+=("-DLOGOS_PROTOCOL_ROOT=${logosProtocol}")
    # Qt splits its host TOOLS into separate packages that must run on the
    # BUILD machine; -DQT_HOST_PATH=<qtbase> cannot reach them. Empty natively.
    ${pkgs.lib.concatMapStringsSep "\n    "
        (f: "cmakeFlagsArray+=(\"" + f + "\")")
        (pkgs.logosQtCrossCmakeFlags or [ ])}
  '';

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
  ];

  meta = with pkgs.lib; {
    description = "Shared runtime library for loading Logos UI modules (LogosQmlBridge, ViewModuleHost, ui-host)";
    platforms = platforms.unix ++ platforms.windows;
    license = licenses.mit;
  };
}
