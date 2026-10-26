cmake_minimum_required(VERSION 3.16)
project(LogosViewModuleRuntime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

option(LOGOS_VIEW_RUNTIME_BUILD_TESTS "Build unit tests for logos-view-module-runtime" OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Network RemoteObjects Qml Gui Quick)
if(LOGOS_VIEW_RUNTIME_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()

if(NOT DEFINED LOGOS_CPP_SDK_ROOT)
    message(FATAL_ERROR "LOGOS_CPP_SDK_ROOT must be defined")
endif()
if(NOT DEFINED LOGOS_QT_HOST_ROOT)
    message(FATAL_ERROR "LOGOS_QT_HOST_ROOT must be defined")
endif()
if(NOT DEFINED LOGOS_PROTOCOL_ROOT)
    message(FATAL_ERROR "LOGOS_PROTOCOL_ROOT must be defined")
endif()

message(STATUS "logos-cpp-sdk : ${LOGOS_CPP_SDK_ROOT}")
message(STATUS "logos-qt-host : ${LOGOS_QT_HOST_ROOT}")
message(STATUS "logos-protocol: ${LOGOS_PROTOCOL_ROOT}")

# Link the SDK stack via exported CMake targets so the consumer inherits
# INTERFACE_INCLUDE_DIRECTORIES + INTERFACE_LINK_LIBRARIES (OpenSSL,
# Boost::system, nlohmann_json). Since the B1 split the Qt HOST RUNTIME
# (LogosAPI and the provider objects) lives in logos-plugin-qt as
# logos-qt-host, which carries the logos-protocol library transitively; the
# Qt-free base SDK contributes header-only types via
# logos-cpp-sdk::logos_headers.
find_package(logos-protocol REQUIRED CONFIG
    PATHS ${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol
    NO_DEFAULT_PATH)
find_package(logos-qt-host REQUIRED CONFIG
    PATHS ${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host
    NO_DEFAULT_PATH)
find_package(logos-cpp-sdk REQUIRED CONFIG
    PATHS ${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk
    NO_DEFAULT_PATH)

# A REQUIRED find_package that resolves to the wrong prefix would still leave
# the target undefined and every LogosAPI symbol undefined at link time — with
# an error far from the cause. Say it here instead.
if(NOT TARGET logos-qt-host::logos_qt_host)
    message(FATAL_ERROR
        "logos-qt-host::logos_qt_host is not a target after "
        "find_package(logos-qt-host) at ${LOGOS_QT_HOST_ROOT}. "
        "LOGOS_QT_HOST_ROOT must point at a logos-plugin-qt "
        "packages.<system>.logos-qt-host prefix.")
endif()

# ── Static library: LogosQmlBridge + ViewModuleHost ─────────────────────────
add_library(logos_view_module_runtime STATIC
    src/LogosQmlBridge.cpp
    src/ViewModuleHost.cpp
    include/LogosQmlBridge.h
    include/ViewModuleHost.h
    include/LogosViewPlugin.h
    include/LogosViewReplicaFactory.h
    include/LogosIntent.h
    include/LogosIntentRouter.h
)

target_include_directories(logos_view_module_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(logos_view_module_runtime PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
    Qt6::Qml
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
)

set_target_properties(logos_view_module_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── Shared library: LogosViewSurface (RFC-7 channel transport, RFC-8 API) ──
# Both ends of the channel SurfaceTransport live here: the host session
# Basecamp links, and the client session ui-host loads with QLibrary only when
# spawned with --surface-transport channel (so the headless ui-host still has
# no Qt6::Quick in its process). Qt6::GuiPrivate is needed for <rhi/qrhi.h>,
# the public-but-private QRhi API that the slot storage renders through.
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
set(LOGOS_VIEW_SURFACE_SOURCES
    surface/SurfaceProtocol.h
    surface/SurfaceExport.h
    surface/SurfaceChannel.h
    surface/SurfaceChannel.cpp
    surface/SlotStorage.h
    surface/ShmSlotStorage.cpp
    surface/SurfaceHostSession.h
    surface/SurfaceHostSession.cpp
    surface/SurfaceProducer.h
    surface/SurfaceProducer.cpp
    surface/SurfaceClientSession.h
    surface/SurfaceClientSession.cpp
    surface/SurfaceClientApi.h
    surface/SurfaceClientApi.cpp
    include/logos/view_surface.h
)
if(WIN32)
    list(APPEND LOGOS_VIEW_SURFACE_SOURCES surface/D3D11SlotStorage.cpp)
endif()
if(APPLE AND NOT IOS)
    enable_language(OBJCXX)
    list(APPEND LOGOS_VIEW_SURFACE_SOURCES surface/IOSurfaceSlotStorage.mm)
endif()

add_library(LogosViewSurface SHARED ${LOGOS_VIEW_SURFACE_SOURCES})
target_compile_definitions(LogosViewSurface PRIVATE LOGOS_VIEW_SURFACE_LIBRARY)
target_include_directories(LogosViewSurface PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/surface>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/logos/surface>
)
target_link_libraries(LogosViewSurface
    PUBLIC Qt6::Core Qt6::Network Qt6::Gui Qt6::Quick
    PRIVATE Qt6::GuiPrivate
)
if(WIN32)
    target_link_libraries(LogosViewSurface PRIVATE d3d11 dxgi)
endif()
if(APPLE AND NOT IOS)
    target_link_libraries(LogosViewSurface PRIVATE "-framework IOSurface" "-framework Metal" "-framework Foundation")
endif()
set_target_properties(LogosViewSurface PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    VERSION 1.0.0
    SOVERSION 1
)

# ── ui-host executable ──────────────────────────────────────────────────────
add_executable(ui-host ui-host/main.cpp)

target_include_directories(ui-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/surface   # SurfaceClientApi.h typedefs only (RFC-7); not linked
)
add_dependencies(ui-host LogosViewSurface)

# Qt6::Gui for the opt-in --gui mode (RFC-1); Qt6::Quick is deliberately not
# linked -- a plugin that renders brings its own.
target_link_libraries(ui-host PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
)

set_target_properties(ui-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(APPLE)
    set_target_properties(ui-host PROPERTIES
        INSTALL_RPATH "@executable_path/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
elseif(UNIX)
    set_target_properties(ui-host PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# ── Install ─────────────────────────────────────────────────────────────────
install(TARGETS logos_view_module_runtime ARCHIVE DESTINATION lib)
install(TARGETS LogosViewSurface
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin)
install(TARGETS ui-host RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
# Host-side headers Basecamp compiles against (the module-facing header is
# include/logos/view_surface.h, installed with the rest of include/).
install(FILES
    surface/SurfaceProtocol.h
    surface/SurfaceExport.h
    surface/SurfaceChannel.h
    surface/SlotStorage.h
    surface/SurfaceHostSession.h
    surface/SurfaceProducer.h
    surface/SurfaceClientSession.h
    surface/SurfaceClientApi.h
    DESTINATION include/logos/surface)

# ── Tests ───────────────────────────────────────────────────────────────────
if(LOGOS_VIEW_RUNTIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
