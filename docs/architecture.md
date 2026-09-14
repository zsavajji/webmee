# Architecture

## Why this engine

Verified in the source and on the device, because this decision gets
re-litigated otherwise.

### Why the WPE port, not GTK or Qt

WPE is WebKit's **embedded** port: no windowing toolkit is compiled in, and the
backend is a *pluggable* interface (`libwpe`). That is the whole reason this
project is possible — `backend/n9-wpe-backend.c` implements six interfaces
(renderer host, view backend, EGL target, offscreen target, pasteboard) against
raw X11/EGL.

The GTK port would have required GTK3 and its dependency tree on a 2009 glibc,
and assumes windowing integration we cannot provide on a device whose portrait
orientation exists only inside Qt's rotated scene graph.

### Why 2.44.4 and not something newer

**It is the last release where Cairo is the rasteriser.** From 2.46 the WPE and
GTK ports switched to Skia. In this tree:

```
Source/cmake/OptionsWPE.cmake:365:  SET_AND_EXPOSE_TO_BUILD(USE_CAIRO TRUE)
skia references under Source/cmake/:  0
```

`USE_CAIRO` is set unconditionally — it is not an option that can be turned back
on in a later release. That single fact decides the version:

- Cairo rasterises on the **CPU** via pixman, and Cairo, pixman, FreeType,
  HarfBuzz and fontconfig all cross-build against glibc 2.10 — which is what
  `deps/build-graphics.sh` does. Skia brings its own build assumptions and a
  far heavier GPU path.
- This GPU is an SGX530: GLES2 only, **64 fragment uniform vectors**, 2048 max
  texture, and a shader compiler that **segfaults on dynamic uniform array
  indexing** (see [device.md](device.md)). WebKit's Cairo-era TextureMapper
  shaders already exceeded what it can compile — a loop had to be unrolled to
  stop the crash. Skia's shader complexity is in a different league.
- 2.44 still supports the classic libwpe backend API
  (`-DENABLE_WPE_PLATFORM=OFF`). Newer releases push toward WPEPlatform, which
  assumes more of the host system.

So 2.44.4 is not "as recent as we managed" — it is the **newest engine that can
still be built as a CPU rasteriser driven by a hand-written backend**, which is
the only shape that fits this hardware.

### Why not Chromium

The floors are concrete, even though the conclusion is judgement:

| | |
|---|---|
| **Kernel** | 2.6.32 (2009). Chromium's sandbox needs seccomp-bpf (3.5+) and user namespaces; there is no supported unsandboxed renderer on Linux. |
| **glibc** | 2.10.1 (2009), far below anything Chromium targets. |
| **Memory** | ~150 MB free. `WPEWebProcess` peaks at 116–127 MB here and that is already tight; Chromium's per-renderer baseline is well beyond it. |
| **Architecture** | 32-bit armv7 **Linux** (not Android) is not a supported Chromium configuration — that is a port, not a build. |
| **Build model** | WebKit's CMake build let us substitute our own versions of 30+ dependencies against an ancient sysroot. Chromium's GN build assumes its own bundled sysroot and a modern host toolchain. |

There is also a continuity argument: the device's own browser is **QtWebKit 2.2**
(WebKit circa 2011), so this is a ~13-year jump *within the same engine family*.
The integration points — Cairo, FreeType, HarfBuzz, GLib, libsoup — are ones
this platform's era already understands.

### What this costs us

We are pinned to the last Cairo configuration, so this branch will not receive
further upstream work in that shape. Moving to 2.46+ means Skia, and on this GPU
that is likely a dead end — the driver already fails on shaders far simpler than
Skia's. Anyone revisiting this should re-read the GLSL compiler findings in
[device.md](device.md) before assuming a newer WebKit is an upgrade.

## Two processes, two toolchains

| | shell | engine |
|---|---|---|
| binary | `/opt/n9browser/bin/WebMee` | `/opt/n9browser/bin/webmee-engine` |
| built with | Harmattan SDK GCC 4.4 | cross GCC 14.2, C++20 |
| links | Qt 4.7.4, QtDeclarative, QtOpenGL | WPE WebKit 2.44.4, GLib, libsoup3 |
| source | `shell/` | `app/`, `backend/` |

The split is not a design flourish — it is forced. WebKit 2.44 needs a modern
C++ toolchain; the device's Qt is GCC 4.4. Linking them into one process would
mix two incompatible C++ ABIs. So the shell is built against the device's real
Qt, the engine against a modern cross toolchain, and they talk over a pipe
([protocol.md](protocol.md)).

WebKit itself then forks `WPEWebProcess` and `WPENetworkProcess`, so a running
browser is four processes.

## How the page reaches the screen

**The problem.** Qt produces portrait by *rotating its scene* inside a landscape
854×480 X window. An X window cannot be rotated. So parenting the engine's
window into the shell's window — the obvious approach — leaves the page stuck in
landscape at a meaningless offset. This was tried and abandoned.

**The solution.** The engine keeps its own X window and its own GPU
compositing. The shell *samples* that window as a texture:

1. The engine creates its X window and, **before mapping it**, redirects it with
   `XCompositeRedirectWindow(..., CompositeRedirectManual)`. Doing it before the
   map is what stops the page flashing unrotated across the screen at startup;
   only the window's own client can act that early, since the shell has not yet
   been told the window id.
2. The engine reports the window id on its event channel (`window <xid>`).
3. The shell names the backing pixmap (`XCompositeNameWindowPixmap`), wraps it
   in an `EGLImageKHR` (`EGL_NATIVE_PIXMAP_KHR`) and binds it as a GLES2 texture
   with `glEGLImageTargetTexture2DOES`.
4. A `QDeclarativeItem` (`shell/webviewitem.cpp`) draws that texture using
   `painter->combinedTransform()` — so Qt's rotation applies to the page exactly
   as it does to every other item.

Zero copies. The engine still renders straight to the GPU.

Measured facts this depends on (see [device.md](device.md)):

- The texture **tracks the window without rebinding** — bind once, later frames
  appear by themselves.
- The pixmap's origin is **top-left**: the item's top-left corner samples `v=0`.
  A flat-colour test cannot show this; a scissored split can.

### Consequences

- The engine's window position is irrelevant (a redirected window renders to its
  pixmap regardless), so it is parked at 0,0. Only the *size* matters.
- Only one client may redirect a window, so the shell must **not** redirect —
  the engine owns it.
- A dead web process leaves a pixmap of undefined memory, which on this tiler is
  routinely the previous framebuffer. The shell therefore drops the texture when
  the engine reports a crash, rather than showing a rotated ghost of its own UI.
- Nothing is drawn at all until the first frame arrives, for the same reason.

## Viewport: device scale factor, not page zoom

The panel is 480×674 device pixels for the page area. Left alone, a site sees
480 CSS px and picks a tablet layout at unreadable size.

WPE 2.44 exposes only `webkit_web_view_set_zoom_level` (page zoom), which
**relayouts the document** — it changes the page rather than presenting it. So
the viewport is set with the **device scale factor** instead:

```
dispatch_set_size(320, 449)              logical, i.e. CSS pixels
dispatch_set_device_scale_factor(1.5)    scale = panel_width / 320

  => css 320x449, rendered into 480x674 device pixels
```

Phone layout *and* crisp text, with no reflow. `wpe_view_backend_dispatch_set_size`
takes the **logical** size; WebKit multiplies by the scale factor to get the
surface.

**The size must be reported by the backend, not computed by the engine.** The
backend is the only component WebKit actually tells. Deriving it elsewhere
produced a rounding disagreement (674 ÷ 1.5 × 1.5 = 673) that made the shell
reject every pixmap and show a black page.

## Pinch: magnify by rendering more pixels

The same mechanism gives sharp pinch zoom without reflow. At magnification *m*,
device size and scale factor rise **together** so the CSS viewport is constant:

```
device  480m x 674m
scale   1.5m
  => css stays 320x449 — WebKit sees no viewport change, so nothing relayouts
```

Showing the panel's 480×674 device pixels out of that larger surface is then a
1:1 mapping, so text is sharp.

In the shell, magnification is a **window into the texture**, not a bigger quad:
vertices always fill the item and the texture coordinates select the visible
part, kept in *normalised* coordinates so they stay valid when the engine swaps
in a differently-sized surface. During the gesture you see the current surface
stretched (immediate, soft); on release the engine re-renders (sharp).

**Capped at 3×**: `674 × 3 = 2022` fits under `MAX_TEXTURE_SIZE` 2048, `674 × 4`
does not. The engine clamps independently and reports the size it actually used,
so the shell cannot drift out of step with it.

Touch coordinates map through that window into surface device pixels; WebKit
divides by the scale factor to get CSS pixels. So taps land correctly at any
magnification and pan offset, and the engine needs to know nothing about either.

## The chrome takes space from the drawing, not from the viewport

Both halves hide together on scroll down and return on scroll up, as the stock
browser did: the URL bar at the top, the platform toolbar at the bottom. The
engine's surface is **never resized by either** — every resize relayouts the
page, which costs far more here than the chrome is worth. Hiding stays a pure
animation with no engine involvement.

The bar does **not** overlay the page, though. `WebViewItem::topInset` is how
many item pixels of it are currently on screen — bound to `urlBar.y +
urlBar.height`, so it follows the hide animation frame by frame — and the page
quad starts there:

```
quad      drawn into item rows topInset..h
texture   surface rows 0..(h - topInset), 1:1, nothing squashed
touch     item y - topInset, through the same mapping
```

The page's first row therefore lands just below the bar instead of under it.
Before this, the top ~72 px of every page was both hidden and untappable at
scroll 0 — which is exactly where consent dialogs and modals put their buttons.

`vFraction()` is the one mapping both `paint()` and `toSurface()` go through.
Keep it that way. If the drawn offset and the touched offset ever disagree, taps
land on something other than what they point at and nothing on screen says so.

The cost moved to the other end: while the bar is up, the bottom `topInset` rows
of the surface are rendered and not shown. Scrolling down hides the bar and
reveals them. On a page that cannot scroll at all — one whose modal consumes
`touchmove`, see the `axis` gap in [protocol.md](protocol.md) — they stay
unreachable.

### The toolbar is the platform's, and it resizes the content area

`PageStackWindow` anchors its content area with
`bottomMargin: toolBar.height`, and the hidden `ToolBar` state is
`height: 0`. So `showToolBar: false` does not slide the toolbar away over the
page — it **grows the page area underneath it**, which is a surface resize and a
relayout on every scroll direction change. That is why this was left undone for
so long.

The way out is to cancel it out rather than fight it. The web view carries
`anchors.bottomMargin: -toolBarHeight`, so as the platform takes the toolbar's
height off the content area the item adds exactly the same amount back:

```
content area   W - T        (T animates to 0 as the toolbar hides)
web view       (W - T) + T  = W, constant, whatever T is
bottomInset    T            what the toolbar still covers
```

The item's height never changes, so `geometryChanged` settles back to the size
the engine already has and the pending resize is cancelled. Track the **live**
`pageStack.toolBar.height` as a `real`, not an int: the platform's margin is
fractional mid-animation and an int would not cancel it exactly.

One consequence is worth knowing: the surface is now the full window height
rather than the height above the toolbar, ~10% more pixels per frame, and the
viewport gains those CSS pixels permanently. That is the point — it is the
vertical room the modals needed — but it is paid on every frame, not only while
the toolbar is hidden.

The same fall-through applies here as to the URL bar: the toolbar's
click-eating `MouseArea` stops mouse events and not touch, since only the web
view sets `acceptTouchEvents`. `inPage()` drops presses outside
`topInset..pageBottom()` for both.

Two smaller things fall out of the same wiring. The platform hides the toolbar
outright while the software input panel is up (`HiddenImmediately`), which used
to churn the item's height and is now free — and it hands the page those rows
to draw into while typing. And the toolbar's own transition is 500 ms against
the URL bar's 180, so the page replaces its `platformStyle` with a
`ToolBarStyle` that matches; otherwise the top snaps away and the bottom oozes.

Scroll position arrives as a `scroll` event (see [protocol.md](protocol.md)).

## Text input and the on-screen keyboard

Three pieces, all of which are needed:

1. **Focus detection** — a `WebKitInputMethodContext` subclass in the engine
   reports `notify_focus_in`/`out`. Nothing else reports focus.
2. **A focus target in the shell** — an off-screen `TextInput` with
   **`opacity: 0`, not `visible: false`**: an invisible QML item cannot take
   focus, and without focus the software input panel has nothing to open for.
3. **Text back into the page** — via the context's `committed` signal for
   characters, and real key events for editing keys.

The caret is kept clear of the keyboard by measuring the overlap in the shell:
WebKit does not know the keyboard exists. The caret rectangle (`imerect`) and
`inputContext.softwareInputPanelRect` are converted to CSS pixels — the only
unit both sides agree on — and the difference is sent as `scrollby`. Measurement
waits 400 ms because the panel animates open and its rectangle is not final when
focus arrives.

## Input

The shell takes real `QTouchEvent`s (`sceneEvent`) rather than the synthesised
mouse events the platform generates, because pinch is invisible in a
single-point stream. One finger is forwarded as raw touch; two fingers become a
pinch and are not forwarded.

WPE's own `TouchGestureController` turns raw touchpoints into clicks, scrolls
and context menus, so the shell recognises **nothing** except pinch. Mouse
handlers remain as a fallback for the case where touch delivery is not set up on
the viewport; they are disabled once a real touch event has been seen.

`QGraphicsView` only routes touch to items when its **viewport widget** has
`Qt::WA_AcceptTouchEvents`.

## Resize handling

Resizing the surface resizes WebKit's viewport, which relayouts. The virtual
keyboard churns the item's height, so resizes are **debounced 250 ms**, and a
size that returns to what the engine already has **cancels** the pending resize
rather than leaving it queued.

A resize orphans the pixmap, and a freshly named one holds undefined memory
until WebKit paints into it. So `acquirePixmap()` never releases the current
texture up front: it names the new pixmap, checks it is the expected size
(within a few pixels — see the rounding note above), and only then swaps.

## Memory

The device has ~120–350 MB free. WebKit's default limit is system RAM, which
would be OOM territory.

`webkit_memory_pressure_settings_*` applies to the **network process only** —
the docs are explicit and the name is misleading. WPE 2.44 exposes no equivalent
for the web process.

The shell runs its own pressure watcher on the `/syspart` cgroup eventfd
(`shell/memorywatcher.cpp`) and sheds caches in stages. The cache model is
`WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER`, not `WEB_BROWSER`, which caches "a very
large number of resources" — the wrong trade here.

## Storage

| what | where | how |
|---|---|---|
| HTTP cache, cookies | `~/.cache/WebMee`, `~/.local/share/WebMee` | `WebKitNetworkSession`, created explicitly |
| favourites | `~/.config/WebMee/bookmarks.conf` | QSettings INI |
| history | `~/.config/WebMee/history.conf` | QSettings INI, capped at 400 |
| settings | `~/.config/WebMee/settings.conf` | QSettings INI |
| passwords | signond | index of ids/usernames only in `~/.config/WebMee` |

The engine **must** be given an explicit `WebKitNetworkSession`. Without one the
view falls back to `webkit_network_session_get_default()`, which is persistent
but uses WebKit's own directories — so the cache silently worked while the UI
reported on a directory nothing ever wrote to.

Clearing goes *through WebKit* (`webkit_website_data_manager_clear`), never by
deleting files: WebKit holds them open, and deleting underneath it corrupts the
index rather than emptying it.

## Known gaps

- No tab implementation behind the tabs UI (single view).
- Password UI (offer-to-save, fill-on-navigate) is not wired; the store works.
- No AIA fetching, so a server that omits an intermediate certificate fails
  where desktop browsers recover.
- Downloads are modelled but not driven by the engine.
- `zoom` (page zoom) remains in the protocol but nothing drives it; pinch uses
  `scale` instead.
- **Scrolling inside a page element that consumes touch does not work** (cookie
  banners, modals). The `axis` command was added for this and does not work as
  written — see [protocol.md](protocol.md). While the chrome is up its rows are
  rendered and not shown, and on such a page nothing can scroll them into view.
