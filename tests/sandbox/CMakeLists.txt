# ui_qml sandbox-escape regression test: a real malicious Qt QML plugin that
# QmlSandbox::configure must refuse to load, while a pure-QML module still loads.

# The "evil" Qt QML plugin (attacker payload). A genuine Qt plugin with valid
# metadata so Qt will dlopen() it when a qmldir 'plugin' directive resolves to
# it. Its constructor writes a sentinel file.
add_library(evil_plugin MODULE evil_plugin/evil_plugin.cpp)
target_link_libraries(evil_plugin PRIVATE Qt6::Core Qt6::Qml)
set_target_properties(evil_plugin PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/evil")
if(APPLE)
    set_target_properties(evil_plugin PROPERTIES SUFFIX ".so") # match Qt unix suffix probing
endif()
set(EVIL_PLUGIN_PATH "${CMAKE_CURRENT_BINARY_DIR}/evil/libevil_plugin.so")

add_executable(tst_qml_sandbox tst_qml_sandbox.cpp)
target_compile_definitions(tst_qml_sandbox PRIVATE
    EVIL_PLUGIN_PATH="${EVIL_PLUGIN_PATH}"
    EVIL_APP_DIR="${CMAKE_CURRENT_SOURCE_DIR}/evil_app")
target_link_libraries(tst_qml_sandbox PRIVATE
    logos_view_module_runtime Qt6::Core Qt6::Qml Qt6::Quick Qt6::Network Qt6::Test)
add_dependencies(tst_qml_sandbox evil_plugin)

# One process per slot: Qt caches loaded QML plugins process-wide, so an
# earlier load could mask a later escape.
foreach(slot
    evilPluginReallyExecutes_whenUnsandboxed
    sandboxBlocksNativePlugin_relativePath
    sandboxBlocksNativePlugin_absolutePath
    sandboxAllowsPureQmlModule
    sandboxDeniesNetworkAccess
    sandboxDeniesFileSchemeNetworkRequest
    sandboxBlocksRemoteSchemeUrl
    sandboxBlocksFileOutsideRoots
    sandboxAllowsFileUnderInstallDir
    sandboxPassesThroughNonExistentProbe
    sandboxAllowsQrcScheme
    evilApp_allProbesBlocked
    evilApp_qmldirPluginImportRejected)
  add_test(NAME qml_sandbox.${slot} COMMAND tst_qml_sandbox ${slot})
  set_tests_properties(qml_sandbox.${slot} PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
