# Shell ↔ engine protocol

One command per line on the engine's **stdin**, one event per line on its
**stdout**. Diagnostics go to **stderr only** — anything a component prints to
stdout is parsed as an event.

UTF-8, `\n`-terminated, no escaping: every message has a fixed arity and the
last field may contain spaces. The shell keeps any trailing partial line for the
next read (`BrowserEngine::onEngineOutput`).

**A line may end in a space, and both sides must preserve it.** `text ` carrying
a single space is how the space bar reaches a page — the shell sends one
keystroke at a time — so neither end may trim a line, only strip its terminator.
`g_strchomp()` is the wrong tool and cost every space in the browser once; see
[gotchas.md](gotchas.md).

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
| `authok <b64user> <b64pass>` | Answer the pending HTTP authentication challenge. Both fields are base64: a password routinely contains a space, which this protocol cannot carry anywhere but the last field. |
| `authcancel` | Answer it with no credentials. The page then loads the server's 401 body. |
| `contextmenu <x> <y>` | Raise the page's context menu at a point, in surface device pixels. The shell times the long press itself: WPE's gesture controller only decides a press was long when the finger **lifts**, so a menu driven by it cannot appear until release. |
| `dlsave <uri>` | Download a url the user picked (the image menu's Save), rather than one a page navigated to. |
| `dlcancel <uri>` | Stop a running download. Keyed by uri, which is what the shell's model is keyed by. |
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
| `ime <0\|1>` | Whether an editable is focused — a **state**, not an edge, and the only signal the on-screen keyboard has. Repeats are suppressed. Reported on focus in/out, and also whenever the focused field is destroyed rather than blurred: at `WEBKIT_LOAD_COMMITTED` and on web-process termination. |
| `imerect <x> <y> <w> <h>` | Caret rectangle in CSS pixels, for keeping the keyboard off it. |
| `link <uri>` | A link was long-pressed. The shell draws its own menu; WPE has no menu UI. |
| `image <uri>` | An image was long-pressed. A *linked* image reports `link` instead — the link wins, as in the stock browser. |
| `dlstart <total> <b64path> <uri>` | A download began. The engine picked the path: it has the server's suggested name and must give WebKit a destination before data arrives. |
| `dlprogress <received> <total> <uri>` | Throttled to one line per 64 KiB. `received-data` fires per chunk, and this pipe also carries frames. |
| `dlfinish <b64path> <uri>` / `dlfail <b64error> <uri>` | Terminal. A cancel arrives as `dlfail`. |
| `auth <retry> <host[:port]> <realm>` | A server asked for HTTP credentials. The port is appended only when it is not the scheme's own. `realm` is the server's own name for the protected area, is last because it is free text, and may be empty. `retry` means the previous answer was rejected. The engine is holding the request and the page is not loading until `authok` or `authcancel` arrives. |

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

**WebKit reports focus-out when a page blurs a field, but not when the document
carrying it is replaced.** Submitting a form is exactly that: the field is
focused, Return navigates, the old document goes away, and no focus-out is ever
emitted — so a shell that closes the keyboard on `ime 0` leaves it up forever.
Measured by driving the engine standalone against a page with a focused input
and then issuing `load`:

```
ime 1                      field focused
loading 1 / url about:blank   navigated away
loading 0                  ...and no ime 0
```

The engine therefore reports it itself, from `WEBKIT_LOAD_COMMITTED` rather than
`WEBKIT_LOAD_STARTED`. A started load can still be cancelled, and claiming
focus-out for a field that is in fact still focused is the worse failure: WebKit
does not emit focus-in again for a field that never lost focus, so the keyboard
would never come back.

## Tabs are addresses, not running pages

The engine knows nothing about tabs. There is one web view and there can only be
one — `WPEWebProcess` peaks at 116–127 MB against the ~150 MB free — so the
shell keeps a list of addresses and switching is a plain `load`. All tabs share
one back/forward history, and switching re-fetches.

A row carries `tabScroll` as well, so a reader coming back to a tab lands where
they left off rather than at the top. It is banked from `engine.scrollY` before
`currentTab` moves — after, and it would be written to the wrong row — and
applied as one `scrollby` when `loading` goes 0, which measured correct on a
real page (`en.m.wikipedia.org`, restored to 900 px) as well as a synthetic one.
It carries a `pendingTab` so that switching away again mid-load cannot drop one
tab's position into another tab's page. Note the role has to be declared in the
model's first `ListElement`: a QML1 `ListModel` fixes its roles there, and
`setProperty` for a role that was never declared is silently dropped.

**This only works because "JavaScript off" no longer means the JS engine is
off** — scroll is reported by an injected user script and applied through
`evaluate_javascript`, both of which were dead in the default configuration.
See [gotchas.md](gotchas.md).

### What was broken, and what was tried

Three silent failures, all in the shell:

- **Nothing ever wrote `tabUrl` back.** A row kept the url it was created with,
  so switching to a tab loaded that — `about:blank` for the first one, which is
  what "the tab lost its context" was. This was the whole bug. The engine is the
  only thing that knows where a page actually went (a redirect, a link, a form
  post), so the row now follows `url`/`title` events rather than the address bar.
- **`tabTitle` was set to the url at creation and never updated.**
- **The thumbnails were an `Image` on `image://tab/<index>`, and no such
  provider was ever registered.** An unknown provider renders nothing and warns
  about nothing, so the cards just looked empty. There is no cheap real one —
  the page is a GL texture in the engine's X window, so a thumbnail means
  `glReadPixels` off an SGX tiler plus a held QImage per tab. The cards carry
  the title and the address instead.

**Per-tab history via `WebKitWebViewSessionState` was built and reverted.** The
idea was sound — park the outgoing tab's state, restore the incoming one, and
each tab keeps its own back/forward list and scroll position with only one view
live. It does not work. `webkit_web_view_restore_session_state()` is for a view
that has not loaded yet; repeatedly re-pointing one live view that already has a
running web process fights the engine. Measured on device: a new web process and
a new X window per switch, a tab coming back showing *another tab's* page, and
`Error sending IPC message: Bad file descriptor`. WebKit's own comment calls
separating restore from navigation a FIXME, which is the warning that was there
to read.

If per-tab history is wanted, the honest shape is a second view, and that does
not fit in the memory budget — see [process-swap.md](process-swap.md), which is
about the same budget being blown by something else.

## Long press, and four things WPE does not do

Long press on a link or an image is a menu the shell draws. Getting the gesture
to arrive at all needed three WebKit patches (`deps/build-webkit.sh`) and one
fix here, each hiding the next:

1. **Our own clock was a counter.** `dispatch_touch` advanced `touch_time += 16`
   per EVENT while the comment claimed milliseconds. `TouchGestureController`
   measures real durations against it — 500 ms for a long press, 200 ms for
   scroll capture — so a press held a full second read as 16 ms and was always
   a click. Use `g_get_monotonic_time()`.
2. **WPE generates the gesture and discards it.** `PageClientImpl`'s handler for
   `ContextMenuEvent` is an empty lambda carrying *"FIXME: Generate
   contextmenuevent without accidentally generating mouseup/mousedown events"*.
   Patched to synthesise a **right**-button press, which is what keeps that
   FIXME honest: WebCore raises the menu from button 2 and a page does not treat
   it as an activation.
3. **...with a pointer motion first.** Upstream's click path moves the pointer
   before pressing; without it WebCore hit-tests wherever it last thought the
   pointer was, and raises no menu for the node under the finger.
4. **The proxy throws the menu away.** `WebContextMenuProxyWPE` overrides
   `show()` with `{ }` — and the BASE `WebContextMenuProxy::show()` is what calls
   `getContextMenuFromProposedMenu`, which reaches the GLib client and emits the
   signal. Removing the override is the fix. `showContextMenuWithItems` stays
   empty on purpose: that is the step that draws a *native* menu, and ours is
   QML.

So **WPE 2.44 ships the entire context-menu API inert** — the signal,
`WebKitContextMenu`, `WebKitHitTestResult`, `ENABLE_CONTEXT_MENUS=1`, all
present, and no touch long press can ever reach any of it.

**The handler takes TWO arguments, not three.** The GTK port and WPE's pre-2022
API pass a GdkEvent in the middle, and `WebKitWebViewWPE1.cpp` declares the
signal that way — but this is the 2022 GLib API, so `WebKitWebViewWPE2.cpp` is
what compiles and the event parameter is omitted from both the signal and the
emit. Declaring the extra parameter shifts everything along, and the hit test
lands in the `user_data` slot: the handler sees a plausible NULL rather than
crashing.

**The menu appears on the timer, not on release.** Upstream returns its
`ContextMenuEvent` from the `up` branch, so anything driven by it waits for the
finger to lift. The shell runs its own 500 ms timer — the same threshold, so the
two cannot disagree — and asks for the menu with `contextmenu <x> <y>`. Movement
past the slop, a lift, a second finger or a pan all cancel it.

WPE then raises its own menu on release anyway, so a second `link` arrives for
the same press. The shell ignores any that comes in while a menu or dialog is
already up.

**Any motion event kills a long press.** The controller promotes a press to a
scroll when `deltaTime >= scrollCaptureThreshold` — an **or** beside the
distance test, so a motion of *zero pixels* does it once 200 ms have passed. A
finger resting on glass emits a stream of them. The shell swallows motion until
the finger leaves an 8 px slop. Measured: no motion → long press fires; one
motion at the identical coordinates → it never does.

Verified on device: short tap navigates; long press reports `link` and does not
navigate; a bare image reports `image`; a linked image reports `link`.

## Downloads

Nothing downloaded before this. There was no `decide-policy` handler, so a link
to a file WebKit cannot display was silently dropped, and the shell's
`DownloadManager` was a model driven by a fake timer that nothing ever called.

It has to be the engine: Qt 4.7.4's `QSslSocket` cannot complete a handshake
with a modern server, so anything the shell fetched over https would fail.

- `decide-policy` turns an undisplayable response into a download.
- `download-started` on the network session wires up `decide-destination`,
  `received-data`, `finished`, `failed`.
- The engine picks the destination — fixed at `/home/user/MyDocs/Downloads`,
  collisions disambiguated `file-1.zip`. One constant, passed shell → engine as
  `N9_DOWNLOAD_DIR` rather than hardcoded on both sides.
- Cancel stops the transfer *before* the partial file is removed. Deleting a
  file the engine is still writing only gets it recreated.

Measured on device with a 14 MB jpeg: 14,679,474 bytes, byte-exact against the
server's `Content-Length`, valid JPEG start and end markers, 221 progress lines
(one per 64 KiB). The saved name is `Fronalpstock_big.jpg` — WebKit's suggested
filename drops the query string, so a url ending `?utm_source=...` does not
become part of it.

Images land in `Downloads`, so the Gallery will not show them until something
tells Tracker about them.

## HTTP authentication

The WPE port's default `authenticate` handler is `return FALSE`
(`WebKitWebViewWPE.cpp`). There is no built-in dialog and nothing else in the
port supplies one, so an unhandled challenge is answered with no credentials
and the site's 401 loads. That is what a password-protected site looked like
here: not an error, a page saying Unauthorized.

The engine keeps a ref to the request and returns TRUE, which is WebKit's
documented way to handle the signal asynchronously, then waits for exactly one
answer. **Every path out of the shell's prompt must send one** — until it does,
that load is parked.

Only the schemes a username and a password can answer are claimed (default,
basic, digest, NTLM, negotiate). Client-certificate and PIN challenges are left
to propagate; returning TRUE for those would park a request the prompt has no
way to satisfy.

Credentials are supplied with `WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION`.
Without that, WebKit asks again for every protected subresource on the page and
again on the next page of the site: the network process only consults its
credential store for `ForSession` and `Permanent` entries
(`NetworkDataTaskSoup.cpp`). `Permanent` additionally wants a secret store this
device does not have, and nothing reports its absence.

Cancelling sends `authcancel`, which answers with **no credentials** rather than
calling `webkit_authentication_request_cancel()`. Cancelling the request cancels
the page load and produces a network error; answering with nothing loads the
server's own 401 body, which is what other browsers show and says considerably
more.

`authok` is the one command the engine does not trace to stderr — the shell
keeps a copy of that log on disk.

### Remembering them

The shell's `PasswordStore` (signond) backs the prompt, so credentials survive a
restart where WebKit's `ForSession` storage does not. The flow:

- A challenge that is **not** a retry, for a host the index knows, is looked up
  and answered **without the prompt ever opening**. Only the index lookup is
  synchronous; signond is D-Bus, so the prompt is held back until it answers and
  a 4 s timer falls through to prompting if it never does.
- A **retry** forgets the stored credential first, whoever supplied it — it was
  just rejected. Without that, every later visit would auto-fill the same wrong
  password and prompt one round trip later. Forgetting makes the store
  self-correcting, which is what allows saving on accept rather than waiting to
  learn the password was right.
- The reply carries the store's **normalised host key**, not what the server
  said, and it is checked against the pending challenge before being used. The
  engine answers an outstanding challenge with nothing when a second one
  arrives, so a lookup can land after its challenge is gone — and answering the
  new one with the old one's password would hand a site someone else's
  credentials.

Settings → Storage counts them and forgets them all; a store that can only be
added to is not one to put a password in.

### The prompt is a modal over an item that still receives touch

A popup does not stop touches reaching the page. Only `WebViewItem` sets
`acceptTouchEvents`, so QGraphicsScene delivers a touch down through every
plain QML item above it — the same fall-through the URL bar needed `inPage()`
for, except a dialog sits in the middle of the screen where `inPage()` cannot
help. Tapping a field in the prompt would also tap whatever the page has under
it. `BrowserPage` therefore disables the web view item while any popup is up; a
disabled `QGraphicsItem` receives no events, and `enabled` does not affect
painting, so the page keeps rendering behind the dialog.

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
