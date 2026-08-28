# logos-view-module-runtime

Shared runtime for hosting Logos **view modules** (Qt/QML UI plugins) in a child
process, isolated from the main application.

This repo exists so that `logos-basecamp`, `logos-standalone-app`, and any
future Logos host application can link the same library and use the same
`ui-host` binary instead of each one carrying its own copy.

## What's in here

- **`logos_view_module_runtime`** — static C++ library, linked into host
  applications (or their plugins). Provides:
  - `LogosQmlBridge` — `QObject` exposed to QML as `logos`. Routes
    `callModule` / `callModuleAsync` to **backend** modules via `LogosAPI`
    (IPC); results are serialized to JSON strings so QML always sees a string.
    **View** modules are reached through `logos.module(name)` /
    `logos.model(name)`, which hand back a typed replica built by that module's
    `LogosViewReplicaFactory` plugin. Calling `callModule` on a view module is
    refused with an error payload, not routed.
  - `LogosIntent.h` — the **frozen** app-to-app intent vocabulary: the six
    error codes, the intent-name grammar, the payload rules and the result
    envelope. Header-only and not a `QObject`, so both a view module and a
    host's broker include the same definitions and cannot disagree about what
    an error code means. See "App-to-app intents" below.
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
│   QML  ──logos.module()────▶         │   QRemoteObjectHost      │
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
  -DLOGOS_CPP_SDK_ROOT=/path/to/logos-cpp-sdk \
  -DLOGOS_QT_HOST_ROOT=/path/to/logos-qt-host \
  -DLOGOS_PROTOCOL_ROOT=/path/to/logos-protocol
cmake --build build
cmake --install build --prefix ./out
```

All three roots are required — the build stops with `FATAL_ERROR` if any is
undefined. `LOGOS_CPP_SDK_ROOT` must point at an installed `logos-cpp-sdk`
(provides `logos_api.h` and `liblogos_sdk`), `LOGOS_QT_HOST_ROOT` at
`logos-qt-host`, and `LOGOS_PROTOCOL_ROOT` at `logos-protocol`.

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
if (!host->spawn("my_view_module", "/path/to/my_view_module.so", authToken)) {
    qWarning() << "Failed to start view module host";
}
```

From QML:

```qml
import QtQuick
Item {
    // A view module is a typed replica, not a JSON call. Properties, slots and
    // signals are reached directly; `callModule` on this name is refused.
    property var backend: logos.module("my_view_module")

    Text { text: backend.someProperty }

    Component.onCompleted: {
        // Slots that return a value hand back a pending call — logos.watch()
        // resolves it, replacing QtRemoteObjects.watch().
        logos.watch(backend.getStatus(), function (result) {
            console.log(result);
        });
    }
}
```

`callModuleAsync` is for **backend** modules, where the result really is a JSON
string:

```qml
logos.callModuleAsync("my_backend_module", "getStatus", [], function (payload) {
    console.log(JSON.parse(payload).value);
});
```

## App-to-app intents

One app asks for a capability; the shell decides who services it. This repo owns
the **frozen half** of that surface — the part apps compile against — and
nothing else. Resolution, consent and dispatch are host policy and live in the
host (in Basecamp, `IntentBroker`).

Three members on the bridge, plus `LogosIntent.h`:

```qml
// Ask. You never name a provider, and never learn which apps are installed.
logos.request("wallet.send", { to: "0xabc", amount: 12.5 }, function (res) {
    if (res.ok) console.log(res.data.txHash)
    else        console.log(res.error)   // one of six codes
})

// Answer, if you declared `provides` in metadata.json.
Connections {
    target: logos
    function onIntentRequested(requestId, intent, params, requesterName) {
        logos.respond(requestId, true, { txHash: "0x…" }, "")
    }
}
```

Frozen means these signatures do not change: `request`, `respond`,
`intentRequested`, the codes in `LogosIntent.h`, and the payload bounds. A host
may replace everything behind them.

Points a host implementer has to honour, because the surface assumes them:

- **`respond` takes all four arguments.** A provider that omits `error` on a
  failure path must not fall into reporting success, so there are no defaults.
- **`requesterName` is host-attested.** The router knows who called by
  construction; it is not read from the payload, and a caller cannot forge it.
- **Payloads are plain data only** — `isCanonicalPayload()` bounds depth,
  size and type, and refuses `QObject*` and `QJSValue`. That is what stops one
  app handing another a live handle into its engine. `respond` flattens
  engine-bound values on the way out for the same reason.
- **A provider may only report `cancelled`, `timeout`, `failed` or
  `bad_request`.** `normalizeError()` coerces anything else, because
  `not_declared` and `unavailable` carry meaning a provider is not entitled to
  assert — both reveal whether a provider exists at all.
- **Every request terminates exactly once**, asynchronously, even on immediate
  failure — for as long as the requester is there to hear it. If its engine is
  torn down or hot-reloaded first, the pending callbacks are dropped uninvoked
  and the broker is told via `intentsAbandoned()`. That is deliberate: running a
  callback against torn-down JS is worse than not answering.

Full reference: `logos-basecamp/docs/app-to-app-intents.md` and
`logos-tutorial/guide-intents-for-app-developers.md`.

## Dependencies

- Qt 6: `Core`, `Qml`, `RemoteObjects`
- `logos-plugin-qt`'s `logos-qt-host` (for `LogosAPI` / `logos_api.h`) —
  the Qt host runtime, linked as `logos-qt-host::logos_qt_host`
- `logos-protocol` (`token_manager.h`, `module_proxy.h`, `remote_transport.h`,
  …) — carried transitively by `logos-qt-host`
- `logos-cpp-sdk` (header-only types, via `logos-cpp-sdk::logos_headers`)

That's it — deliberately no dependency on `logos-liblogos`, `logos-module`, or
any specific module repo, so this runtime stays a thin shared layer.
