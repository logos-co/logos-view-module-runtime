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

# Readiness replay for QML hot reload. m_replicas is only ever populated through
# module(), which loads a factory plugin — so covering the branch where replay
# meets a genuinely Valid replica needs a real factory plugin and a real
# published source, not just the empty/dropped maps the unit suite can build.
add_library(test_replica_factory_plugin MODULE test_replica_factory_plugin.cpp)
target_link_libraries(test_replica_factory_plugin PRIVATE
    Qt6::Core
    Qt6::RemoteObjects
)
target_include_directories(test_replica_factory_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

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

# The bridge takes a filesystem path to the factory plugin, so the test needs
# to know where CMake put it.
target_compile_definitions(test_logos_qml_bridge_replay PRIVATE
    TEST_REPLICA_FACTORY_PLUGIN="$<TARGET_FILE:test_replica_factory_plugin>"
)
add_dependencies(test_logos_qml_bridge_replay test_replica_factory_plugin)

add_test(NAME LogosQmlBridgeReplayTests COMMAND test_logos_qml_bridge_replay)

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
