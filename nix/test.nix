{ pkgs, logosSdk }:

pkgs.stdenv.mkDerivation {
  pname = "logos-view-module-runtime-check";
  version = "1.0.0";

  src = ../.;

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsHook
  ];

  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    pkgs.qt6.qtdeclarative
    logosSdk
  ];

  dontStrip = true;

  preConfigure = ''
    export MACOSX_DEPLOYMENT_TARGET=12.0

    mkdir -p logos-cpp-sdk/include/cpp logos-cpp-sdk/include/core logos-cpp-sdk/lib
    cp -r ${logosSdk}/include/cpp/* logos-cpp-sdk/include/cpp/
    cp -r ${logosSdk}/include/core/* logos-cpp-sdk/include/core/
    for ext in dylib so a; do
      f="${logosSdk}/lib/liblogos_sdk.$ext"
      [ -f "$f" ] && cp "$f" logos-cpp-sdk/lib/
    done

    cmakeFlagsArray+=("-DLOGOS_CPP_SDK_ROOT=$PWD/logos-cpp-sdk")
  '';

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Debug"
    "-DLOGOS_VIEW_RUNTIME_BUILD_TESTS=ON"
  ];

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export QT_QPA_PLATFORM=offscreen
    ctest --output-on-failure
    runHook postCheck
  '';

  # Emit a marker so Nix has something to install; the real purpose of this
  # derivation is to run the unit tests during checkPhase.
  installPhase = ''
    mkdir -p $out
    echo "tests-passed" > $out/result
  '';

  meta = with pkgs.lib; {
    description = "Unit tests for logos-view-module-runtime (LogosQmlBridge)";
    platforms = platforms.unix;
    license = licenses.mit;
  };
}
