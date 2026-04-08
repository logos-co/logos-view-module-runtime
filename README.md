# logos-view-module-runtime

Shared runtime for hosting Logos **view modules** (Qt/QML UI plugins) in a child
process, isolated from the main application.

This repo exists so that `logos-basecamp`, `logos-standalone-app`, and any
future Logos host application can link the same library and use the same
`ui-host` binary instead of each one carrying its own copy.

## What's in here

- **`logos_view_module_runtime`** — static C++ library, linked into host
  applications (or their plugins). Provides:
  - `LogosQmlBridge` — `QObject` exposed to QML as `Logos`. Routes
    `callModule(module, method, args)` calls either to a regular backend module
    via `LogosAPI` (IPC), or to a view module via a private
    `QRemoteObjectDynamicReplica`. Results are serialized to JSON strings so
    QML always sees a string.
  - `ViewModuleHost` — spawns a `ui-host` child process for a given view
    module plugin, generates a unique local socket name, watches stdout for
    `READY`, and emits `ready()`. The parent then points `LogosQmlBridge` at
    that socket via `setViewModuleSocket(name, socket)`.

- **`ui-host`** — standalone executable. Loads a single Qt plugin
  (`--path <plugin.so>`), calls `initLogos(LogosAPI*)` on it via reflection
  (`QMetaObject::invokeMethod`), and then exposes a QObject on a
  `QRemoteObjectHost` at the socket given by `--socket`. Remoting strategy:
  - **Typed remoting (preferred)**: if the plugin declares the
    `LogosViewPlugin` interface (from `logos-plugin-qt`) via
    `Q_INTERFACES(LogosViewPlugin)` so that `qobject_cast<LogosViewPlugin*>`
    succeeds, `ui-host` calls
    `viewPlugin->enableRemoting(&host)`. The generated
    `<Foo>ViewPluginBase` (produced by `logos_module(REP_FILE …)` in
    `logos-plugin-qt`) invokes `host->enableRemoting<FooSourceAPI>(backend)`
    so typed replicas on the client side reach the `Valid` state. The
    remoted object is `viewPlugin->viewObject()`.
  - **Dynamic remoting (fallback)**: for plugins without a `.rep` /
    `LogosViewPlugin` implementation, `ui-host` falls back to
    `host.enableRemoting(pluginObject, moduleName)`, which propagates all
    `Q_INVOKABLE`s, slots, signals, and `Q_PROPERTY`s (with `NOTIFY`) via a
    `QRemoteObjectDynamicReplica` on the client side.

  Any `Q_PROPERTY` on the remoted object whose value is a
  `QAbstractItemModel*` is additionally remoted as a child source named
  `<moduleName>/<propertyName>`. Prints `READY` once it's listening.

## View object convention

A view module plugin keeps its plugin-lifecycle class separate from the
QObject that QML actually talks to. The preferred path is to inherit the
generated `<Foo>ViewPluginBase` from `logos-plugin-qt` (produced by
`logos_module(REP_FILE my_view.rep …)`), which implements `LogosViewPlugin`
and wires typed remoting:

```cpp
class MyPlugin : public MyViewPluginBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "co.logos.MyPlugin" FILE "metadata.json")
    Q_INTERFACES(PluginInterface LogosViewPlugin)
public:
    Q_INVOKABLE void initLogos(LogosAPI* api) {
        m_backend = new MyBackend(api, this);
    }
    QObject* viewObject() override { return m_backend; }
private:
    MyBackend* m_backend = nullptr;
};
```

`ui-host` calls `viewPlugin->enableRemoting(&host)`, which internally does
`host->enableRemoting<MySourceAPI>(m_backend)` using the typed source
generated from the `.rep` file. QML on the parent side talks to
`MyBackend` via a typed replica.

For plugins without a `.rep` file (no `LogosViewPlugin` implementation),
`ui-host` falls back to dynamic remoting of the plugin object itself — this
keeps legacy modules working unchanged.

## Architecture

```
┌────────────────────────────┐         ┌──────────────────────────┐
│ Host app (basecamp / etc.) │         │ ui-host (child process)  │
│                            │         │                          │
│   QML  ──logos.callModule──▶         │   ViewModuleProxy        │
│           │                │ QRO     │     │                    │
│   LogosQmlBridge ──────────┼────────▶│     ▼                    │
│           │                │ local   │   QPluginLoader          │
│           ▼                │ socket  │     │                    │
│   ViewModuleHost ──spawn──▶│         │     ▼                    │
│                            │         │   <view module>.so       │
└────────────────────────────┘         └──────────────────────────┘
```

Each view module gets its own `ui-host` process and its own private socket, so
a crash or hang in one view module cannot take down the host app or other
view modules.

Non-view backend modules continue to use the existing `LogosAPI` IPC path
unchanged — `LogosQmlBridge` only switches to QRO when the requested module
name was previously registered via `setViewModuleSocket`.

## Building

### Nix (recommended)

```sh
nix build .#default
```

Outputs:
- `result/lib/liblogos_view_module_runtime.a`
- `result/include/` — public headers
- `result/bin/ui-host`

### CMake (manual)

```sh
cmake -S . -B build -GNinja \
  -DLOGOS_CPP_SDK_ROOT=/path/to/logos-cpp-sdk
cmake --build build
cmake --install build --prefix ./out
```

`LOGOS_CPP_SDK_ROOT` is required and must point at an installed
`logos-cpp-sdk` (provides `logos_api.h` and `liblogos_sdk`).

## Consuming from another repo

In the consumer's `flake.nix`:

```nix
inputs.logos-view-module-runtime.url = "github:logos-co/logos-view-module-runtime";
```

Pass the package into the consumer's app derivation and forward it as a CMake
variable:

```nix
cmakeFlags = [
  "-DLOGOS_VIEW_MODULE_RUNTIME_ROOT=${logosViewModuleRuntime}"
];
```

In the consumer's `CMakeLists.txt`:

```cmake
target_include_directories(my_app PRIVATE ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/include)
target_link_directories(my_app PRIVATE ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/lib)
target_link_libraries(my_app PRIVATE logos_view_module_runtime)
```

The `ui-host` binary should be copied into the app's `bin/` directory at
install time so `ViewModuleHost` can `QProcess::start("ui-host", ...)` it:

```nix
cp ${logosViewModuleRuntime}/bin/ui-host $out/bin/ui-host
```

## Using the bridge

```cpp
auto* api = new LogosAPI(/* ... */);
auto* bridge = new LogosQmlBridge(api, this);
engine.rootContext()->setContextProperty("logos", bridge);

// For a view module, spawn its host process and wire the bridge to its socket
auto* host = new ViewModuleHost(this);
connect(host, &ViewModuleHost::ready, this, [bridge, host] {
    bridge->setViewModuleSocket("my_view_module", host->socketName());
});
if (!host->spawn("my_view_module", "/path/to/my_view_module.so")) {
    qWarning() << "Failed to start view module host";
}
```

From QML:

```qml
import QtQuick
Item {
    Component.onCompleted: {
        // Prefer the async form for view modules — the sync callModule() blocks
        // the QML/JS event loop while waiting for the QRO reply.
        logos.callModuleAsync("my_view_module", "getStatus", [], function(payload) {
            const result = JSON.parse(payload);
            console.log(result.value);
        });
    }
}
```

## Dependencies

- Qt 6: `Core`, `Qml`, `RemoteObjects`
- `logos-cpp-sdk` (for `LogosAPI` / `logos_api.h`)

That's it — deliberately no dependency on `logos-liblogos`, `logos-module`, or
any specific module repo, so this runtime stays a thin shared layer.
