cmake_minimum_required(VERSION 3.16)
project(LogosViewModuleRuntime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

option(LOGOS_VIEW_RUNTIME_BUILD_TESTS "Build unit tests for logos-view-module-runtime" OFF)

find_package(Qt6 REQUIRED COMPONENTS Core RemoteObjects Qml)
if(LOGOS_VIEW_RUNTIME_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()

if(NOT DEFINED LOGOS_CPP_SDK_ROOT)
    message(FATAL_ERROR "LOGOS_CPP_SDK_ROOT must be defined")
endif()

message(STATUS "logos-cpp-sdk : ${LOGOS_CPP_SDK_ROOT}")

include_directories(
    ${LOGOS_CPP_SDK_ROOT}/include
    ${LOGOS_CPP_SDK_ROOT}/include/cpp
    ${LOGOS_CPP_SDK_ROOT}/include/core
)

link_directories(${LOGOS_CPP_SDK_ROOT}/lib)

# ── Static library: LogosQmlBridge + ViewModuleHost ─────────────────────────
add_library(logos_view_module_runtime STATIC
    src/LogosQmlBridge.cpp
    src/ViewModuleHost.cpp
    include/LogosQmlBridge.h
    include/ViewModuleHost.h
    include/LogosViewPlugin.h
    include/LogosViewReplicaFactory.h
)

target_include_directories(logos_view_module_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(logos_view_module_runtime PUBLIC
    Qt6::Core
    Qt6::RemoteObjects
    Qt6::Qml
    logos_sdk
)

set_target_properties(logos_view_module_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── ui-host executable ──────────────────────────────────────────────────────
add_executable(ui-host ui-host/main.cpp)

target_include_directories(ui-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ui-host PRIVATE
    Qt6::Core
    Qt6::RemoteObjects
    logos_sdk
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
