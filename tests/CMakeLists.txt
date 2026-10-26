# Unit tests for logos-view-module-runtime (Qt Test)

add_executable(test_logos_qml_bridge
    test_logos_qml_bridge.cpp
)

target_link_libraries(test_logos_qml_bridge PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
)

add_test(NAME LogosQmlBridgeUnitTests COMMAND test_logos_qml_bridge)

# End-to-end bridge test: real provider + LogosAPI + QJSEngine async chain.
add_executable(test_logos_qml_bridge_e2e
    test_logos_qml_bridge_e2e.cpp
)

target_link_libraries(test_logos_qml_bridge_e2e PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeE2ETests COMMAND test_logos_qml_bridge_e2e)

# GUI reproduction: the async chain inside a real QQuickView scene on a
# QGuiApplication event loop (offscreen).
add_executable(test_logos_qml_bridge_gui
    test_logos_qml_bridge_gui.cpp
)

target_link_libraries(test_logos_qml_bridge_gui PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeGuiTests COMMAND test_logos_qml_bridge_gui)

# Regression guard for the QML-only `result`-type serialization bug (makeResult
# -> null). serializeResultForTesting must emit the canonical {success,value,error}.
add_executable(test_logos_qml_bridge_result
    test_logos_qml_bridge_result.cpp
)

target_link_libraries(test_logos_qml_bridge_result PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeResultTests COMMAND test_logos_qml_bridge_result)

# App-to-app intent surface: the frozen three symbols, the callback contract
# (exactly once / always async / real JS objects), the responsibility boundary
# on respond(), and the metaobject trust boundary. Null LogosAPI throughout —
# no sockets, no ui-host, no packages.
add_executable(test_logos_qml_bridge_intents
    test_logos_qml_bridge_intents.cpp
)

target_link_libraries(test_logos_qml_bridge_intents PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeIntentTests COMMAND test_logos_qml_bridge_intents)

# Capability-handshake reproduction: un-pre-seeded target token + real
# requestModule round-trip against a published capability_module provider.
add_executable(test_logos_qml_bridge_handshake
    test_logos_qml_bridge_handshake.cpp
)

target_link_libraries(test_logos_qml_bridge_handshake PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeHandshakeTests COMMAND test_logos_qml_bridge_handshake)

# Per-plugin identity: a bridge built for an identity presents tokens from that
# identity's ISOLATED store, so an undeclared backend costs a real
# capability_module handshake and is refused — where a host-identity bridge
# reached it with no handshake at all.
add_executable(test_bridge_identity
    test_bridge_identity.cpp
)

target_link_libraries(test_bridge_identity PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME BridgeIdentityTests COMMAND test_bridge_identity)

# ── 2-process reproduction (cross-process boundary) ─────────────────────────
# Two standalone executables (driven manually, not via ctest): the modules host
# publishes echo + capability in one process; the bridge driver runs the call
# chains in another, so every call crosses the QtRO process boundary — the
# QML-only topology basecamp actually uses.
add_executable(test_2proc_modules_host test_2proc_modules_host.cpp)
target_link_libraries(test_2proc_modules_host PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_executable(test_2proc_bridge_driver test_2proc_bridge_driver.cpp)
target_link_libraries(test_2proc_bridge_driver PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Network
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

# Deferred event-subscription regression: a QML plugin subscribing in
# Component.onCompleted BEFORE the target core module is reachable. The
# subscription must survive and arm when the module appears (including a
# mid-session install), must not double-deliver, and must never block.
add_executable(test_logos_qml_bridge_deferred
    test_logos_qml_bridge_deferred.cpp
)

target_link_libraries(test_logos_qml_bridge_deferred PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeDeferredTests COMMAND test_logos_qml_bridge_deferred)

# The CALL path against a module that is not reachable yet — the sibling of the
# deferred-subscription suite above. callModule/callModuleAsync carried the
# identical `if (!client->isConnected())` guard, which was dead code for years
# and so had no coverage in either direction. Pins the two forms' DIFFERENT
# contracts: the synchronous one waits a bounded time and then names
# callModuleAsync in its error; the asynchronous one holds the call until the
# module appears, bounded by the caller's own deadline.
add_executable(test_logos_qml_bridge_calls
    test_logos_qml_bridge_calls.cpp
)

target_link_libraries(test_logos_qml_bridge_calls PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeCallsTests COMMAND test_logos_qml_bridge_calls)

# Readiness replay for QML hot reload, against a real published source: the
# unit suite can only build empty/dropped maps, never a Valid replica.
add_executable(test_logos_qml_bridge_replay
    test_logos_qml_bridge_replay.cpp
)

target_link_libraries(test_logos_qml_bridge_replay PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeReplayTests COMMAND test_logos_qml_bridge_replay)

# Dynamic replicas end to end: readiness before any QML, .rep enums through the
# Logos.<Source> singleton, and a backend replaced by one with different enum
# values (a reinstall) being seen by the next engine.
add_executable(test_logos_qml_bridge_dynamic
    test_logos_qml_bridge_dynamic.cpp
)

target_link_libraries(test_logos_qml_bridge_dynamic PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

# Backends are published through repc-generated sources in real modules, which
# put fully qualified enum types on the wire; plain QObject sources do not.
qt6_add_repc_sources(test_logos_qml_bridge_dynamic test_rep_backend.rep)

add_test(NAME LogosQmlBridgeDynamicTests COMMAND test_logos_qml_bridge_dynamic)

# ── Module teardown hook (ui-host) ──────────────────────────────────────────
#
# Three fixture plugins that differ ONLY in how they answer aboutToUnload(),
# and one test that spawns the real ui-host on each through the real
# ViewModuleHost. See test_unload_fixture.cpp for why they keep a journal.
#
# Built from one source with UNLOAD_MODE switching the behaviour, so the three
# cannot drift into differing in anything else -- which is the whole basis for
# comparing their journals against each other.
foreach(_mode_name_pair "none:0" "finishes:1" "hangs:2")
    string(REPLACE ":" ";" _pair ${_mode_name_pair})
    list(GET _pair 0 _name)
    list(GET _pair 1 _mode)
    set(_tgt test_unload_fixture_${_name})
    add_library(${_tgt} MODULE test_unload_fixture.cpp)
    target_link_libraries(${_tgt} PRIVATE Qt6::Core)
    target_compile_definitions(${_tgt} PRIVATE UNLOAD_MODE=${_mode})
endforeach()

add_executable(test_ui_host_unload test_ui_host_unload.cpp)

target_link_libraries(test_ui_host_unload PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Network
    Qt6::Test
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
)

target_compile_definitions(test_ui_host_unload PRIVATE
    FIXTURE_PLUGIN_NONE="$<TARGET_FILE:test_unload_fixture_none>"
    FIXTURE_PLUGIN_FINISHES="$<TARGET_FILE:test_unload_fixture_finishes>"
    FIXTURE_PLUGIN_HANGS="$<TARGET_FILE:test_unload_fixture_hangs>"
)

add_dependencies(test_ui_host_unload
    ui-host
    test_unload_fixture_none
    test_unload_fixture_finishes
    test_unload_fixture_hangs
)

# ViewModuleHost::spawn() resolves the ui-host binary as a SIBLING of the
# running executable (QCoreApplication::applicationDirPath()). That is the
# production layout, and the test drives the production spawn path, so the test
# binary has to live in bin/ beside it rather than in the tests dir.
set_target_properties(test_ui_host_unload PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME UiHostUnloadTests COMMAND test_ui_host_unload)

# ── GUI mode and surface handshake (ui-host --gui, RFC-1 / RFC-2) ──────────
#
# One fixture plugin that records the application class it was given, the
# app_id ui-host set from the surface token, and whether the QQuickWindow it
# opens is exposed; one test that spawns the real ui-host on it through the
# real ViewModuleHost, in gui mode and plain.
add_library(test_gui_fixture MODULE test_gui_fixture.cpp)
# The fixture is a MODULE: it includes the header-only <logos/view_surface.h>
# (RFC-8) and links nothing of LogosViewSurface, as a real module would not.
target_include_directories(test_gui_fixture PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_gui_fixture PRIVATE Qt6::Core Qt6::Gui Qt6::Quick)

add_executable(test_ui_host_gui test_ui_host_gui.cpp)

target_link_libraries(test_ui_host_gui PRIVATE
    logos_view_module_runtime
    LogosViewSurface
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Test
    Qt6::RemoteObjects
    logos-qt-host::logos_qt_host
    logos-cpp-sdk::logos_headers
)

target_compile_definitions(test_ui_host_gui PRIVATE
    FIXTURE_PLUGIN_GUI="$<TARGET_FILE:test_gui_fixture>"
)

add_dependencies(test_ui_host_gui ui-host test_gui_fixture LogosViewSurface)

# Same reason as test_ui_host_unload: ui-host is resolved beside the executable.
set_target_properties(test_ui_host_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME UiHostGuiTests COMMAND test_ui_host_gui)
# The child inherits this; ui-host only overrides it when --surface-socket is
# given. The software scene graph is chosen by the fixture itself.
set_tests_properties(UiHostGuiTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ── Channel SurfaceTransport (RFC-7 / RFC-8), two processes ─────────────────
#
# test_surface_view is the VIEW: a QGuiApplication that goes through the C
# API of SurfaceClientApi.h and the header-only module API exactly as ui-host
# and a module would, and journals every input event it is replayed.
# test_surface_channel is the HOST: a SurfaceHostSession that spawns it, reads
# the frames it renders as QImages (shm) and drives geometry, credits, input
# and failure. No GPU, no compositor: QT_QPA_PLATFORM=offscreen, software
# scene graph.
add_executable(test_surface_view test_surface_view.cpp)
target_link_libraries(test_surface_view PRIVATE
    LogosViewSurface
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)
set_target_properties(test_surface_view PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_executable(test_surface_channel test_surface_channel.cpp)
target_link_libraries(test_surface_channel PRIVATE
    LogosViewSurface
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Test
)
target_compile_definitions(test_surface_channel PRIVATE
    SURFACE_VIEW_BINARY="$<TARGET_FILE:test_surface_view>"
)
add_dependencies(test_surface_channel test_surface_view)

add_test(NAME SurfaceChannelTests COMMAND test_surface_channel)
set_tests_properties(SurfaceChannelTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
