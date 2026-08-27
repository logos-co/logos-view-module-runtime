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

# ── ui-host executable ──────────────────────────────────────────────────────
add_executable(ui-host ui-host/main.cpp)

target_include_directories(ui-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ui-host PRIVATE
    Qt6::Core
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
install(TARGETS ui-host RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

# ── Tests ───────────────────────────────────────────────────────────────────
if(LOGOS_VIEW_RUNTIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
