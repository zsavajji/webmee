# Shell ↔ engine protocol

One command per line on the engine's **stdin**, one event per line on its
**stdout**. Diagnostics go to **stderr only** — anything a component prints to
stdout is parsed as an event.

UTF-8, `\n`-terminated, no escaping: every message has a fixed arity and the
last field may contain spaces. The shell keeps any trailing partial line for the
next read (`BrowserEngine::onEngineOutput`).

## Commands (shell → engine)

| command | meaning |
|---|---|
| `load <url>` | Navigate. URL is already normalised by the shell. |
| `back` / `forward` / `reload` / `stop` | History and loading control. |
| `geometry <x> <y> <w> <h>` | Page area size in **device** pixels. `x`/`y` are ignored (the window is redirected offscreen) but kept so the wire format stays stable. Sets the base size from which the logical size and scale factor are derived. |
| `scale <magnification>` | Pinch result, ≥ 1.0. Raises device size and scale factor together so the CSS viewport is unchanged. Clamped by the engine to what `MAX_TEXTURE_SIZE` allows. |
| `zoom <level>` | WebKit page zoom. Relayouts. Currently unused — pinch uses `scale`. |
| `touch <down\|motion\|up> <slot> <x> <y>` | One touchpoint in **surface device pixels**. WebKit divides by the scale factor to get CSS pixels. |
| `set <javascript\|images> <0\|1>` | Runtime settings. |
| `layoutwidth <css px>` | CSS pixels the page lays out to (base 345). Wider means more CSS pixels in **both** directions, so this is also the knob for "the page does not fit vertically". |
| `renderscale <n>` | Device pixels rasterised per CSS pixel. 1.0 = Fast (rasterise small, upscale), 1.5 = Sharp (match the panel 1:1). Does not change layout. |
| `useragent <string>` | Rest of the line, spaces included. |
| `text <utf8>` | Typed text, committed into the focused field. Never contains a newline: the shell splits on them and sends `key 65293` around the pieces, so Enter reaches the page as a key press and can submit a form. |
| `key <keysym> <0\|1>` | An editing key as an xkb keysym (Backspace `65288`, Return `65293`, arrows `65361`–`65364`). |
| `scrollby <dx> <dy>` | Scroll the page, in CSS pixels. Used to lift the caret clear of the keyboard. |
| `axis <x> <y> <dx> <dy>` | A wheel event. **Unproven** — see the note below. |
| `clearcache` / `clearcookies` | Ask WebKit to clear its own storage. |
| `quit` | Exit the main loop. |

## Events (engine → shell)

| event | meaning |
|---|---|
| `ready` | The view exists and commands will be accepted. Anything sent before this is dropped. |
| `window <xid>` | The engine's X window, already redirected. The shell names its pixmap from this. |
| `surface <w> <h>` | The surface size WebKit is actually rendering, in device pixels. **Emitted by the backend**, which is the only component WebKit tells. |
| `frame` | A frame finished rendering. The texture already shows it; this is a wake-up. |
| `url <s>` / `title <s>` | Current document. |
| `progress <0..1>` / `loading <0\|1>` | Load state. |
| `nav <canBack> <canForward>` | History availability. |
| `failed <msg>` | Load failure. `failed web process <reason>` and `failed TLS <flags> (<uri>)` are distinguished by prefix. |
| `scroll <y>` | Scroll position in CSS pixels. Drives the URL bar's hide-on-scroll. Reported by an injected user script through a script message handler — event-driven, throttled to 8 px in the page, `capture: true` so nested scrollers report too. |
| `ime <0\|1>` | The page focused or blurred an editable. The only signal that the on-screen keyboard should open. |
| `imerect <x> <y> <w> <h>` | Caret rectangle in CSS pixels, for keeping the keyboard off it. |

## Text input

The page has no keyboard of its own. WebKit drives a
`WebKitInputMethodContext` for the focused editable; the engine subclasses it
and reports `notify_focus_in` / `notify_focus_out` as `ime 1` / `ime 0`. Nothing
else reports focus, so without this, tapping a field in a page does nothing at
all.

Text goes back in through the context's **`committed` signal**, not synthesised
key events: WebKit inserts the string directly, which handles multi-byte
characters and predictive input that a keysym stream cannot express. Editing
keys still need real key events via `key`.

**Return is an editing key, not text**, even though the keyboard delivers it as
a committed `"\n"`. Committed as text it inserts a line break; a page cannot
submit on it, because nothing it can listen for has happened. `sendText()`
therefore splits on newlines and emits `key 65293` between the pieces. Verified
against `Shared/libwpe/WebEventFactory.cpp`: keysym `0xFF0D` produces
`key: "Enter"`, `windowsKeyCode: VK_RETURN` and text `"\r"`, which is what a
form submit listens for. (`hardware_key_code` stays 0, so `KeyboardEvent.code`
is unset — nothing depends on it here.)

Verified end to end on device: `text Hello` -> `Hello`, `text 42` ->
`Hello42`, Backspace -> `Hello4`.

## Scrolling inside a page element does not work yet

WPE only synthesises scrolling from touch **when the page did not consume the
touch event** (`WPEWebView.cpp`, `handle_touch_event` -> `doneWithTouchEvent`).
A modal that attaches `touchmove` handlers and calls `preventDefault` — every
cookie banner does — can therefore never be scrolled by dragging.

Wheel events bypass that path, so the `axis` command exists. **It does not work
as written**: a test produced zero frames. The 2D smooth axis event most likely
needs a phase (`PhaseBegan`/`Changed`/`Ended`) that `dispatch_axis_event` does
not carry the way the code assumes. Unfinished, do not rely on it.

## Ordering rules that matter

- **`ready` is emitted immediately before the initial `load_uri`.** Anything the
  shell sends in response arrives *after* that load has begun. Settings that must
  apply to the first page therefore come through the **environment**, not the
  protocol (`N9_NO_JS`, `N9_NO_IMAGES`, `N9_USER_AGENT`, `N9_LAYOUT_WIDTH`).

  Having the mechanism is not the same as using it: the shell knew the persisted
  JavaScript setting and passed it only over the protocol, so the first page of
  every launch loaded with scripts enabled however the switch was set. With
  JavaScript now defaulting off that is the difference between the default
  working and not. A setting that must hold for the first page needs an entry in
  **both** the environment block and the `ready` handler — the second is what
  keeps the shell authoritative when the engine is restarted.

- **`wpe_view_backend_dispatch_*` is a silent no-op before WebKit registers as
  the backend client.** libwpe guards every dispatch on `backend->backend_client`.
  The backend's own `view_initialize` runs after registration and is the correct
  place for the initial size.

- **The backend, not the engine, is authoritative on surface size.** The engine
  requests a logical size and a scale; WebKit rounds; the backend reports what
  came out. Computing it on the engine side disagreed by one pixel and the shell
  rejected every pixmap.

- **Settings are applied on leaving the settings page, not as they change.**
  `BrowserEngine::commitSettings()` sends only what differs from what the engine
  was actually told (shadow copies), and sends `renderscale` before
  `layoutwidth` because both end in `apply_surface()` and the other order lays
  the page out twice. Applying each toggle immediately made the browser crawl.

- **`frame` also provides backpressure.** The engine writes it before dispatching
  frame-complete, so a shell that stops draining throttles the engine rather than
  letting it run ahead.

## Environment passed to the engine

Set by `BrowserEngine::startEngine()`.

| variable | why |
|---|---|
| `DISPLAY` | applauncherd gives the app none; taken from `DisplayString(QX11Info::display())` so both processes are on the same X server, which XComposite requires. |
| `LD_LIBRARY_PATH` | Prepends any inherited value — the only way to test a rebuilt library without root, since `DT_RPATH` is searched first. |
| `WPE_BACKEND` | **Not** how libwpe picks the backend (it was compiled with a default and ignores the environment; the variable it would read is `WPE_BACKEND_LIBRARY`). Used solely so the engine can `dlopen` the already-loaded module and `dlsym` `n9_backend_move_resize`. |
| `GIO_MODULE_DIR` | GIO's module path is compiled into GLib and points at the build prefix. |
| `SSL_CERT_FILE` | OpenSSL's `openssldir` is inside our tree, and the device's 2011 CA store predates most roots in use today. |
| `FONTCONFIG_FILE` | Our fonts.conf, not the device's. |
| `N9_VIEW_RECT`, `N9_LAYOUT_WIDTH` | Initial geometry and the CSS width to lay out to (default 320). |
| `N9_NO_JS`, `N9_NO_IMAGES` | Initial content settings, applied before the first load. Sent on **every** launch carrying the persisted value, `1` or `0` — the engine tests the value, not presence, so it can be told "on" as well as "off". `N9_NO_JS=0` is the way to force JavaScript on for a measurement, since it now defaults off. |

## Test hooks

These exist so the app can be exercised without the touchscreen, and are read by
the shell unless noted.

| variable | effect |
|---|---|
| `N9_STARTUP_URL` | Load this instead of `about:blank`. |
| `N9_ENGINE` | Run this engine binary — lets a fresh build be tested against a root-owned installed tree. |
| `N9_VERIFY_PAGE=<n>` | After drawing, `glReadPixels` the shell's own framebuffer and log the values for the first *n* paints **after the first frame**. Distinguishes "not composited" from "screen is off". |
| `N9_LOG_SHADERS`, `N9_LOG_DLSYM`, `N9_SHADER_DUMP` | Read by `probe/n9crash.c`, see [debugging.md](debugging.md). |
