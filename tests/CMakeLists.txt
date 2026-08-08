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
    logos-qt-sdk::logos_qt_sdk
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
    logos-qt-sdk::logos_qt_sdk
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
    logos-qt-sdk::logos_qt_sdk
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
    logos-qt-sdk::logos_qt_sdk
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeResultTests COMMAND test_logos_qml_bridge_result)

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
    logos-qt-sdk::logos_qt_sdk
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeHandshakeTests COMMAND test_logos_qml_bridge_handshake)

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
    logos-qt-sdk::logos_qt_sdk
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
    logos-qt-sdk::logos_qt_sdk
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
    logos-qt-sdk::logos_qt_sdk
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
    logos-qt-sdk::logos_qt_sdk
    logos-cpp-sdk::logos_headers
    logos-protocol::logos_protocol
)

add_test(NAME LogosQmlBridgeCallsTests COMMAND test_logos_qml_bridge_calls)
