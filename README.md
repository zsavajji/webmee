# WebMee

A modern web engine for the Nokia N9 (MeeGo 1.2 Harmattan, 2011).

WPE WebKit 2.44.4 with JIT and GPU compositing, wrapped in a Qt Components
shell that looks like the stock Harmattan browser. It renders contemporary
layouts — flexbox, CSS grid, custom properties — that the device's own
QtWebKit 2.2 cannot.

```
┌──────────────────────────┐        ┌────────────────────────────┐
│ WebMee (shell)           │        │ webmee-engine              │
│ Qt 4.7.4 / QML1          │ stdio  │ WPE WebKit 2.44.4          │
│ GCC 4.4 (Harmattan SDK)  │◄──────►│ GCC 14.2 (cross)           │
│                          │        │  ├─ WPEWebProcess          │
│ draws the page as a      │        │  └─ WPENetworkProcess      │
│ GLES2 texture            │        │                            │
└──────────┬───────────────┘        └────────────┬───────────────┘
           │      XComposite: engine's X window  │
           └──────── redirected, sampled ────────┘
```

## Documentation

| Document | What is in it |
|---|---|
| [docs/architecture.md](docs/architecture.md) | **Why this engine** (WPE 2.44.4, not Chromium or newer), process model, compositing, viewport and zoom design |
| [docs/device.md](docs/device.md) | Measured hardware and OS facts. **Read before assuming anything about this device** |
| [docs/protocol.md](docs/protocol.md) | The shell ↔ engine line protocol, normative |
| [docs/building.md](docs/building.md) | Toolchains, build order, packaging, and the traps in each |
| [docs/debugging.md](docs/debugging.md) | Probes, crash capture without gdb, the host-side test suite, and the env vars that make the app testable |
| [docs/performance.md](docs/performance.md) | What was measured, what helped, what did not, and the harnesses to resume with |
| [docs/qml-components.md](docs/qml-components.md) | Which QML types actually exist on the device, probed rather than assumed |
| [docs/process-swap.md](docs/process-swap.md) | **Open bug.** The browser stalls every few links: a web process per cross-site navigation, measured, with every cheap fix ruled out |
| [docs/gotchas.md](docs/gotchas.md) | Catalogue of everything that cost real time, with the evidence |

## Layout

```
app/        engine process (n9engine.c) + build script
backend/    WPE backend: X11/EGL window, input, crash handler
shell/      Qt/QML shell: UI, texture compositing, stores
deps/       cross-build scripts for the whole dependency stack
probe/      standalone device probes (GL limits, shader replay, crash capture)
test/host/  the shell's logic tested on the build host, no device needed
icon/       icon generator (pure Python, no dependencies)
docs/       this documentation
deploy.sh   assemble + push the on-device tree
package.sh  build the installable .deb
```

## State

JavaScript defaults to **off** — one core, and a page's scripts hold it rather
than burn it (3.6x the frames with it off; see
[docs/performance.md](docs/performance.md)). Settings → JavaScript turns it on
and the choice sticks. "Off" strips scripts from the page at parse time
(`enable-javascript-markup`) rather than shutting down the JS engine, so the
browser's own injected scripts still run — see
[docs/gotchas.md](docs/gotchas.md).

Working and verified on device: flexbox/grid/custom-property layout, JIT,
GPU compositing in portrait, HTTPS, disk cache, touch input with
drag-to-scroll, pinch magnification, favourites, downloads, history
suggestions, memory-pressure shedding, password storage via signond,
HTTP basic/digest authentication.

Known gaps are listed at the end of [docs/architecture.md](docs/architecture.md).
