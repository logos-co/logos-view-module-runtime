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
    `logos.model(name)`, which hand back a dynamic replica built from the
    schema the backend sends — no code from the module is loaded into the
    host, so a reinstalled module is picked up by its next open. `.rep` enums
    are served as a `Logos.<RepClass>` singleton read from that replica.
    Hosts call `prepareViewModule(name, timeoutMs, done)` and create the view
    only once it reports ready. Calling `callModule` on a view module is
    refused with an error payload, not routed.
  - `LogosIntent.h` — the **frozen** app-to-app intent vocabulary: the six
    error codes, the intent-name grammar, the payload rules and the result
    envelope. Header-only and not a `QObject`, so both a view module and a
    host's broker include the same definitions and cannot disagree about what
    an error code means. See "App-to-app intents" below.
  - `ViewModuleHost` — spawns a `ui-host` child process for a given view
    module plugin, generates a unique local socket name, watches stdout for
    `READY <source name>`, and emits `ready()`. The parent then points
    `LogosQmlBridge` at that socket via
    `setViewModuleSocket(name, socket(), sourceName())`.

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
    `logos-plugin-qt`) invokes `host->enableRemoting<FooSourceAPI>(backend)`,
    publishing it under the `.rep` class name. The remoted object is
    `viewPlugin->viewObject()`.
  - **Dynamic remoting (fallback)**: for plugins without a `.rep` /
    `LogosViewPlugin` implementation, `ui-host` falls back to
    `host.enableRemoting(pluginObject, moduleName)`, which propagates all
    `Q_INVOKABLE`s, slots, signals, and `Q_PROPERTY`s (with `NOTIFY`).

  Any `Q_PROPERTY` on the remoted object whose value is a
  `QAbstractItemModel*` is additionally remoted as a child source named
  `<moduleName>/<propertyName>`. Prints `READY <source name>` once it's
  listening — the name the backend was published under, which the parent
  needs to acquire it.

### GUI mode and the surface handshake (`--gui`, `--surface-socket`)

By default `ui-host` is a `QCoreApplication`: a backend has no platform
plugin and cannot open a window, and everything a `ui_qml` module shows is
drawn by its QML inside the host. A module whose view *is* a renderer (a map,
a video, a 3D scene) needs its backend to render in its own process, so
`ui-host` has an opt-in GUI mode:

```
ui-host --name <module> --path <plugin> --socket <qtro socket> [--gui] [--surface-socket <name>] [--surface-transport wayland|channel]
```

- `--gui` constructs a `QGuiApplication` instead (with
  `quitOnLastWindowClosed` off — the process's lifetime stays the parent's).
  Nothing is injected into the plugin; it may create its own `QQuickWindow`
  (or `QWindow` + `QOpenGLContext`) from `initLogos()` on. Without `--gui`
  the binary runs the same code path as before.
- `--surface-socket <name>` (implies `--gui`) names the Wayland socket of the
  host's nested compositor. Before the application is constructed, `ui-host`
  sets `QT_QPA_PLATFORM=wayland` and `WAYLAND_DISPLAY=<name>` for itself and
  clears `DISPLAY`, so its windows reach that compositor and no other display.

The parent chooses the mode with `ViewSpawnOptions`:

```cpp
ViewSpawnOptions opts;
opts.gui = true;                      // or opts.surfaceSocket = compositorSocket;
host->spawn(name, path, authToken, opts);
```

For a GUI spawn `ViewModuleHost` also mints a per-spawn **surface token**
(16 random bytes, hex). It travels on the existing `_token` local socket as a
second line after the auth token:

```
<authToken>\n
SURFACE <surfaceToken>\n
```

`ui-host` sets `QGuiApplication::setDesktopFileName("logos-view-<token>")`
before the plugin runs — Qt's Wayland client uses it as the xdg-shell
`app_id`, which is what a compositor authenticates (together with the child's
pid, `ViewModuleHost::processId()`) — and extends its readiness line to
`READY <source name> SURFACE <token>`. The parent exposes the token it minted
as `ViewModuleHost::surfaceToken()` from the moment `spawn()` returns -- a host
compositor must expect it before the child's first surface commit, which can
precede READY -- and `surfaceTokenConfirmed()` reports whether the child echoed
the same token; a plain spawn leaves both empty/false. The token is never put
on argv or in the environment.

Host code that composites the surface binds a module name to its token
through `LogosQmlBridge::setViewModuleSurface(module, token)` /
`viewModuleSurfaceToken(module)`. Neither is `Q_INVOKABLE`: like the intent
host-side API, their absence from the metaobject is what keeps the token away
from app QML, which sees the bridge as `logos`.

An old `ui-host` rejects `--gui` as an unknown option and exits; the parent
sees `processExited` and the module never becomes ready. A GUI-capable plugin
started by an old host (no `--gui`) should detect
`qobject_cast<QGuiApplication*>(qApp) == nullptr` and degrade rather than
crash. `tests/test_ui_host_gui.cpp` drives both modes against the real
binaries.

### The channel transport and the module API (`--surface-transport`, `LogosViewSurface`)

The nested Wayland compositor above is Linux-only. For every other desktop OS
— and as a fallback on Linux — the runtime ships a second SurfaceTransport
with the same contract (maps RFC-6/RFC-7): the host allocates a three-slot
ring of frame buffers in OS-native shared storage, hands them to the view over
a bounded binary protocol on a `QLocalSocket`, grants one frame of credit at a
time, and the view renders into the granted slot with a `QQuickRenderControl`.
Input travels the other way as replayed `QEvent`s.

| slot kind | storage | copies | where |
|---|---|---|---|
| `shm` | `QSharedMemory`; view reads back, host uploads | 1 | every OS |
| `d3d11` | shared `ID3D11Texture2D` + keyed mutex | 0 | Windows (`D3D11SlotStorage.cpp`, compiled only on `WIN32`; **unverified**) |
| `iosurface` | `IOSurface` + `MTLTexture`, Mach-port handoff | 0 | macOS (`IOSurfaceSlotStorage.mm`, compiled only on `APPLE`; **unverified**) |

Both ends live in **`LogosViewSurface`**, a shared library installed to
`lib/` (`bin/LogosViewSurface.dll` on Windows):

- host side: `logos::surface::SurfaceHostSession` (`surface/SurfaceHostSession.h`) —
  `listen()`, `expect(token, pid)`, `configure(logicalSize, dpr, visible)`,
  `sendInput()`, `takeReadyForDisplay()` / `frameSwapped()` for the scene-graph
  texture node;
- view side: `SurfaceClientSession` + `SurfaceProducer`, reached by `ui-host`
  through the C functions in `surface/SurfaceClientApi.h`.

```
ui-host --name <m> --path <p> --socket <s> --surface-transport channel --surface-socket <server name>
```

```cpp
ViewSpawnOptions opts;
opts.surfaceTransport = "channel";          // or "wayland"; "" = none
opts.surfaceSocket = session->serverName();  // SurfaceHostSession::listen()
host->spawn(name, path, authToken, opts);
```

`ui-host` does not link the library: with `--surface-transport channel` it
`QLibrary`-loads `LogosViewSurface` after the token lines are read, calls
`logos_view_surface_init(token, serverName, "channel")` and only then loads
the plugin, so a headless `ui-host` still never maps `Qt6::Quick`. The
environment is left alone (the producer never shows a window; tests run it
under `QT_QPA_PLATFORM=offscreen`). Under `--surface-socket` without a
transport the library is loaded too, if present, so the same module API works
on both transports.

**Module API (RFC-8).** A `view_surface` module never opens its own window.
It includes the header-only `<logos/view_surface.h>` and asks the runtime:

```cpp
#include <logos/view_surface.h>

QQuickWindow* win = LogosViewSurface::window();    // nullptr on an old runtime
QQuickItem*   root = LogosViewSurface::rootItem(); // parent content here
QString how = LogosViewSurface::transport();       // "wayland", "channel-shm", ..., "none"
```

The functions resolve a versioned C table (`LogosViewSurfaceV1`) that
`ui-host` publishes as the `"logos.viewSurface.v1"` dynamic property of the
`QCoreApplication`; nothing in the module links the library, and an old
runtime — which publishes nothing — makes every call return `nullptr` /
`"none"`, so the module can fall back to a window of its own. Rules: call from
the main thread, from `initLogos()` on; choose
`QQuickWindow::setGraphicsApi()` before the first `window()` call; never
`show()`, `resize()` or delete the window — geometry is the host's. Under the
wayland transport `window()` is a plain `QQuickWindow` shown as a toplevel
(the compositor does the rest); under channel it is the producer's offscreen
window.

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
`MyBackend` via a dynamic replica.

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
    // A view module is a replica, not a JSON call. Properties, slots and
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
