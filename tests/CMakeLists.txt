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
    logos_sdk
)

add_test(NAME LogosQmlBridgeUnitTests COMMAND test_logos_qml_bridge)

add_executable(test_view_module_host
    test_view_module_host.cpp
)

target_link_libraries(test_view_module_host PRIVATE
    logos_view_module_runtime
    Qt6::Core
    Qt6::Test
)

add_test(NAME ViewModuleHostUnitTests COMMAND test_view_module_host)
