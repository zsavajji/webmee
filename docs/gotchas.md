# Gotchas

Everything here cost real time at least once. Each entry states the symptom
first, because that is how you will meet it again.

## Graphics and compositing

**Black page area, nothing drawn.** `QDeclarativeItem` sets
`ItemHasNoContents`, so `paint()` is never called until you clear that flag.

**A QML child hides the page.** Children paint *above* their parent, so a
placeholder `Rectangle` inside the web view item covers it. Give it `z: -1`.

**Nothing draws inside `beginNativePainting()`.** Qt's paint engine leaves a VBO
bound, and with one bound, client-side vertex array pointers are read as buffer
offsets. `glBindBuffer(GL_ARRAY_BUFFER, 0)` first.

**The top of the page is hidden, and tapping it does nothing useful.** The URL
bar is a plain QML `Item`: it accepts neither mouse nor touch, so QGraphicsScene
delivers events over it straight down to the web view, which clicked whatever
the bar was sitting over. The bar's visible height is taken out of what the page
*draws* (`WebViewItem::topInset`) rather than out of the surface, and presses
above it are dropped. Insetting the surface instead would relayout the page on
every show and hide.

**Stale content after a resize, or a rotated copy of the UI in the page area.**
Resizing a redirected window orphans its pixmap, and a freshly named one holds
undefined memory — on this tiler, routinely the previous framebuffer. Never
release a good texture before a correctly-sized replacement has content, and
draw nothing before the first frame.

**...and the same ghost again, on navigation, and a tab that never comes back.**
One cause, two symptoms, and `setEngineWindow()` is where both live.

Two things had to be got right there, and the first hid the second:

1. `m_sawFrame` (what "draw nothing before the first frame" is built on) is a
   **latch** — set once, cleared only at startup and by `pageCrashed()`. A new
   engine window mid-session is the startup situation again, so the latch has
   to be cleared or the ghost is drawn the instant the id changes.
2. **Do not skip the work when the reported id is unchanged.** The engine
   reports a window only from `egl_target_initialize`, immediately after
   creating one, and `egl_target_deinitialize` destroys the previous one —
   which frees its XID for the server to hand out again. An identical id means
   a *different* window, not the same one. Returning early there keeps a pixmap
   named from a destroyed window, which is freed memory, and nothing ever
   re-acquires it: the ghost then never clears, and the page never reappears.

Fixing (1) alone changes nothing whenever the id repeats, because the early
return sits above it. This showed up a few links in and never on the first page,
because at the time WebKit started a new web process — and so a new window — on
every cross-site navigation. That is patched off now (see
[process-swap.md](process-swap.md)), so new windows are rare: a web process
crash and restart is the remaining way to get one. The handling still has to be
right, and it is now much harder to stumble into by browsing.

**Every pixmap rejected, black page.** The logical size is device ÷ scale
*rounded*, so the surface WebKit produces can be a pixel off what either side
predicted (480×673 against an expected 480×674). Compare with tolerance.

**The page renders but the layout is wrong (tablet-width).**
`wpe_view_backend_dispatch_*` is a **silent no-op** before WebKit registers as
the backend client. The backend's own `view_initialize` runs after registration;
anything dispatched earlier is dropped.

**`eglCreateWindowSurface` returns `EGL_BAD_ALLOC` (0x3005).** The window must be
created with the visual from the config's `EGL_NATIVE_VISUAL_ID`.

**`eglChooseConfig` succeeds but returns 0 configs.** This driver has no pbuffer
config. Use a real window.

**"Couldn't open libEGL.so.1".** libepoxy `dlopen`s the standard sonames; the
PowerVR driver ships only unversioned `libEGL.so`. `deploy.sh` creates the
symlinks.

**The web process dies on ordinary pages.** The GLSL compiler segfaults on
dynamic indexing of a uniform array. See [device.md](device.md) — this is a hard
rule for any shader on this GPU.

## X11

**`BadAccess` on redirect.** A window has exactly one redirecting client, and
redirecting twice from the same client is an error. The engine owns the redirect;
the shell only names the pixmap.

**The page flashes unrotated at startup.** Redirect *before* mapping. Only the
window's own client can act that early.

**`XCompositeNameWindowPixmap` on a dead window aborts the app.** Xlib's default
error handler exits. Install a temporary error trap around those calls.

**The whole browser freezes and never recovers, with every process asleep.**
Not overload — check `wchan` before assuming it is. All four processes in `S`
with `utime+stime` frozen and `Cached` still in the hundreds of MB is a
deadlock, not memory pressure. The shell sat in `eventfd_read`: `MemoryWatcher`
created its eventfds with `eventfd(0, 0)` and drained *both* whenever *either*
fired, so a blocking read on the one with a zero counter parked the Qt event
loop until the kernel happened to cross the other threshold. Everything else
followed from that — the shell stopped draining the engine's stdout, the
backpressure `printf("frame\n")` filled the 64 KB pipe and blocked, and WebKit
never received its frame-complete.

Two rules out of it: **an eventfd read from an event loop must be
`EFD_NONBLOCK`**, and **drain only the descriptor that fired** —
`QSocketNotifier::activated(int)` hands you the fd, so use it.

## Process and environment

**The engine dies with "could not create the EGL display".** applauncherd gives
the app no `DISPLAY`. Qt reaches X through the booster; a `QProcess` child does
not. Pass `DisplayString(QX11Info::display())` — the same server matters,
XComposite depends on it.

**The status bar is black on black.** The app must be launched through
`invoker`; applauncherd is what registers the window with the compositor.

**`WPE_BACKEND` does nothing.** libwpe was compiled with a default backend and
ignores the environment; the variable it *would* read is `WPE_BACKEND_LIBRARY`.
The backend actually loaded is whatever the soname resolves to.

**A rebuilt library has no effect.** `DT_RPATH` is searched before
`LD_LIBRARY_PATH` (we link `--disable-new-dtags`), so `/opt` always wins.

**`scp` silently does nothing.** "Text file busy" if the binary is running. Kill
first, and do not redirect scp's stderr to `/dev/null`.

**Settings do not apply to the first page.** `ready` is emitted immediately
before the initial `load_uri`, so anything the shell sends in reply lands after
the load starts. Use the environment for initial state.

## Qt 4 / QML1

**`QFile::atEnd()` is useless on procfs** — size 0 means the loop never runs and
you silently read zeros. Use `fopen`/`fgets`/`sscanf`.

**Opening the keyboard relayouts the page.** `Window.qml` shrinks
`windowContent` by the panel height, so every item inside gets shorter. Cancel it
on the web view with a negative bottom margin and let the keyboard overlay
instead. Read the panel size the way the platform does — in portrait that is
`softwareInputPanelRect.width`, because the rectangle is in unrotated
framebuffer coordinates.

**Hiding the platform toolbar relayouts the page.** `PageStackWindow` anchors
its content area with `bottomMargin: toolBar.height` and the hidden `ToolBar`
state is `height: 0`, so `showToolBar: false` grows the page area rather than
sliding the toolbar over it. The web view cancels that with
`anchors.bottomMargin: -pageStack.toolBar.height` and keeps a constant height.
Track that height as a `real`: the platform's margin is fractional mid-animation
and an int does not cancel it exactly.

**Tapping the address bar clears the selection.** `selectAll()` in
`onActiveFocusChanged` is undone by the tap positioning the cursor afterwards.
Take the first tap with a `MouseArea` that is disabled once focused.

**Touch never reaches items.** `QGraphicsView` routes touch only when its
**viewport widget** has `Qt::WA_AcceptTouchEvents`.

**Long press works when driven by a script and never with a finger.** WPE's
`TouchGestureController` promotes a press to a scroll on any motion once 200 ms
have passed — `deltaTime >= scrollCaptureThreshold` is an **or** alongside the
distance test, so a motion of *zero pixels* is enough. A finger resting on glass
produces a stream of `TouchUpdate`s, so the gesture became a scroll long before
it could reach the 500 ms long-press threshold. Measured on device: no motion →
long press fires; one motion at the identical coordinates → it never does.

The shell therefore swallows motion until the finger leaves a small slop around
the press (`kMoveSlop`). This is also why a synthetic probe is not proof for
touch: it sends exactly the events you write, and a real finger sends more.

**A dialog is open and the tap lands on the page behind it too.** The same
fall-through as the URL bar above: only the web view sets `acceptTouchEvents`,
so QGraphicsScene delivers touch down through the popup's plain QML items. The
URL bar could fix this with `inPage()` because it is at a known edge; a dialog
is in the middle of the screen. `BrowserPage` disables the web view item while
any popup is up (`page.modalUp`). A disabled `QGraphicsItem` gets no events,
and `enabled` does not affect painting, so the page still renders behind it.

**The browser pauses for tens of seconds every few links, then recovers.** Not
overload and not the memory watcher: WebKit starts a web process per cross-site
navigation and four end up alive at once. Count `WPEWebProcess` before blaming
anything else. Fixed by patching process-swap off in
`deps/build-webkit.sh` — if it comes back, check that patch survived a WebKit
re-unpack before looking anywhere else. The measurements and the three closed
runtime routes are in [process-swap.md](process-swap.md).

**`grep -c "literal" <binary>` on the BUILD HOST silently matches nothing.** The
busybox substitute for `strings` works on device and misleads here — GNU grep
treats the file as binary and suppresses the count. Use `strings -a | grep -c`
on the host. Concluding "the symbol is not in the library" the first way is how
a working fix nearly got written off.

**A password-protected site loads a page saying Unauthorized.** WPE's default
`authenticate` handler is `return FALSE` and the port has no dialog of its own,
so the challenge is answered with nothing. The prompt has to come from the
shell; see [protocol.md](protocol.md).

**A GL viewport needs `FullViewportUpdate`**; Qt cannot track dirty regions
across the swap.

**A magnified page will not move sideways.** Magnification deliberately leaves
the CSS viewport alone, so the page is exactly as wide as the window and WebKit
has nothing to scroll horizontally — the extra pixels are in the surface, and
only the shell's `m_viewOrigin.x` selects which slice is drawn. Forwarding the
drag to the engine looks reasonable and does nothing. See
[architecture.md](architecture.md).

**A drag that pans clicks the link it started on.** WPE's
`TouchGestureController` turns a press into a click on release unless the touch
moved `pixelsPerLineStep` or was down 200 ms. A pan holds the surface point
under the finger still by design, so a quick one hits neither threshold. Do not
forward the press until the gesture is known.

**`InfoBanner is not a type`** — needs `import com.nokia.extras 1.1` (1.0 is not
enough).

**A page silently fails to open, with nothing in the log.** A QML type that does
not exist resolves at *creation* time, not load time. `SelectionListItem` is not
a Harmattan component; `SelectionDialog` is. Check
[qml-components.md](qml-components.md), and use `N9_TEST_QML=1` to instantiate
every page at startup instead of waiting for a tap.

**An `Image` shows nothing and warns about nothing.** A url naming an image
provider that was never registered fails exactly like a `image://theme/` typo.
The tab switcher drew `image://tab/<index>` for months against no provider at
all. A missing provider is not a broken image — it is a blank one.

**`DialogStatus.Closed` is 3, not 0.** `Opening` is 0, so
`if (dialog.status)` is false for a dialog that is opening and true for one that
is shut. Compare against the named value, never truthiness.

**`Item.visible` is EFFECTIVE visibility, not the property you bound.**
QtQuick 1 reads it off `QGraphicsItem`, whose hide propagates down and clears
the flag on every child. A control inside a closed dialog therefore reports
`visible: false` however its own binding evaluates. Reading a child's `visible`
to mean "this option is on offer" fails exactly when a dialog handler runs,
because the dialog is closing by then — the auth prompt's "Remember password"
never saved anything until it was changed to test `passwords.available`
directly.

**The suggestion list stays up over the page that just loaded.**
`urlField.focus = false` reads as "drop the focus" and does not do it: the
platform `TextField` keeps `activeFocus`, and anything bound to that — here the
suggestion list's `opacity` — stays as it was. QML focus has to GO somewhere.
Hand it to a plain `Item` that wants nothing (`urlBar.forceActiveFocus()`).

This one had already been "fixed" once with `focus = false` and a comment
explaining the symptom, which is how it survived: the code looked like it
handled the case. It is also invisible to a test that never opens the list —
three assertions passed vacuously here before the stub was complete enough for
the list to appear at all.

**A handler stops halfway with no visible error.** A ReferenceError in a QML
signal handler abandons the rest of the function silently.
`platformCloseSoftwareInputPanel()` is a method **on the TextField**, not a
global — calling it bare from another object's function broke navigation and
history clicks at once.

**Member init order warnings are real.** GCC initialises in *declaration* order,
not initialiser-list order.

## Build and toolchain

**`frexpl` "missing or broken beyond repair".** glibc 2.10 hides all
long-double prototypes under `__NO_LONG_DOUBLE_MATH`, though libm exports the
symbols. Force-include the compat header — and only where needed.

**`strerror_r` has the wrong signature.** A force-included header that pulls in
`<features.h>` before the source sets `_GNU_SOURCE` changes the whole
translation unit. The compat header therefore includes nothing.

**`-include` breaks unrelated builds.** It was captured into libpng's `.S`
files, fontconfig's gperf input, and libtasn1's gnulib make variables. Opt-in
only.

**`std::trunc` is not in `std`.** The cross libstdc++'s C99 math probe failed for
the same long-double reason. Define `_GLIBCXX_USE_C99_MATH_FUNCS` **and**
force-include the compat header; macros alone make it worse.

**`-mthumb` is mandatory** for JSC: it gates both `OFFLINE_ASM_BACKEND` and
`ENABLE_JIT_DEFAULT`.

**`fallocate64` undefined.** glibc 2.10 has `fallocate` but not `fallocate64`,
and meson's `cc.has_function` with no prefix link-tests its own stub and wrongly
succeeds.

**`type '(NULL)'` at runtime, or a second backend registry.** Two copies of a
library with process-global state. GLib, libwpe and libsoup **must** be shared.

**`unresolvable R_ARM_JUMP24 relocation`.** `-Dprefer_static=true` made meson
fall through to the device sysroot's non-PIC GLib 2.28 from 2011.

**Exit 126 running a build tool.** It is the ARM binary. Keep `$PREFIX/bin` off
`PATH` and use host tools.

**`cc1plus` killed.** Host OOM. Build with `-j5`; the host has no swap and
WebCore unified sources take 2–3 GB each.

**Tools "not found" after sourcing `toolchain.sh`.** It derives `$ROOT` from
`$0`; source it only from a script inside the project.

**A full rebuild from a one-line change.** The flags stamp wipes the build
directory when `LDFLAGS` changes — including the rpath, hence `DEVPREFIX`.

**zsh does not word-split `$CFLAGS`.** Build scripts must be `#!/bin/sh`.

**`pkill -f <pattern>` matches its own command line** and kills the shell running
it (exit 144). Use `pgrep -x` or a bracketed class.

## Packaging and device

**The .deb installs on a desktop and fails on the device.** Harmattan's 2011
dpkg cannot read zstd/xz. Use `dpkg-deb -Zgzip`.

**"Manifest cannot have `<! ..>` elements", and the manifest is full of
comments.** Comments are fine. A **double hyphen inside an XML comment** is
illegal XML, and aegis reports the parse failure that way — using `--` as an
em-dash in the rationale is all it takes. Aegis only parses at install time on the device, so this costs a full transfer
to discover; `package.sh` now refuses to build unless the manifest parses.

**"request 'UID::user' for 'applauncherd-launcher::...' ignored, matched by
deny rule".** Harmless, and a sign the line is doing nothing. The
`<for path="applauncherd-launcher::...">` entry propagates **capability**
credentials to the launcher so a boosted app keeps them; identity tokens like
`UID::user` cannot be propagated and hit a deny rule. Add that line when there
is a real capability to carry, not before.

**Unexplained web-process crashes after a deploy.** The injected bundle shipped
to the wrong path because a `cp` flattened `lib/wpe-webkit-2.0/`. WebKit
`dlopen`s it by absolute path, and a missing bundle is not reported as a load
error. `deploy.sh` asserts the critical paths now.

**The keyboard will not go away after pressing Enter in a page field.** WebKit
emits focus-out when a page blurs a field, but not when the document holding it
is replaced — and a form submit does exactly that. No `ime 0`, so nothing ever
tells the shell to close the panel. The engine reports it from
`WEBKIT_LOAD_COMMITTED` instead (not `STARTED`: a cancelled load would leave a
still-focused field with no way to reopen the keyboard, since WebKit will not
emit focus-in for a field that never lost focus).

**Cookies work all session and are gone at the next start.** A persistent
`WebKitNetworkSession` does not imply a persistent cookie jar —
`webkit_cookie_manager_set_persistent_storage()` has to be called or the jar is
memory-only, and nothing says so. It presents as sites forgetting logins.

**HTTPS fails everywhere.** glib-networking is a **runtime GIO module** — nothing
links it, so a missing one produces no build error, just
"TLS support is not available" at runtime. The engine logs the backend at
startup for this reason.

**"Unacceptable TLS certificate" on one site.** Check the *identity*, not just
the chain: `openssl s_client` does not verify the hostname unless you pass
`-verify_hostname`. A site serving its host's default wildcard certificate
genuinely fails in every browser.

**`aegis-dpkg` still needs root** — it is a wrapper that exits unless `id -u` is
0. `develsh` gives gid 0 but uid 29999 and cannot write `/opt`.

**`devel-su` hangs over ssh.** It wants a TTY for the password prompt.

**busybox has no `strings` and no `timeout`.** `grep -c "literal" binary` works.

## Settings that look wired but are not

Four separate controls in this project set a member variable and did nothing
else — cache size, JavaScript, images, and the user agent. The UI existed, the
value persisted, and nothing reached the engine. **When adding a setting, verify
the engine actually received it**, not that the UI updated.

Then the same class of bug turned up at the other end: JavaScript and images
*did* reach the engine and were never written to disk, so they worked perfectly
until the app was restarted. A setting needs **three** things and any one of
them can be missing without a symptom until much later:

1. a write — `persist(key, value)` in the setter,
2. a read — `cfg.value(key, ...)` in `BrowserEngine`'s constructor,
3. delivery to the engine — `commitSettings()`, or the environment for anything
   that must be true for the first page.

The two halves of persistence are checkable without the device, which is the
cheapest test this repo has:

```sh
diff <(grep -o 'persist("[^"]*"'   shell/browserengine.cpp | sed 's/.*("//;s/"//' | sort) \
     <(grep -o 'cfg.value("[^"]*"' shell/browserengine.cpp | sed 's/.*("//;s/"//' | sort)
```

A key on one side only is a setting that does not stick, or dead state in the
file.

Related: **a setting can be persisted, applied, and still miss the first page.**
The environment mechanism for this existed and simply was not used for
JavaScript or images — the shell sent them only over the protocol, which lands
after `load_uri`. It was invisible while the startup URL was `about:blank` and
became load-bearing the moment JavaScript defaulted to off.

**"JavaScript off" must mean the PAGE's scripts, not the JS engine.** The
browser's own features are built out of JavaScript, and
`enable-javascript=FALSE` turns those off too. It takes the injected scroll
reporter with it — so the shell received **zero** `scroll` events, and
`evaluate_javascript` (the `scrollby` command) became a silent no-op. Three
features were dead in the default configuration and each looked like a bug of
its own: the URL bar never hid on scroll, the caret was never lifted clear of
the on-screen keyboard, and a tab's scroll position could not be read back.

`enable-javascript-markup=FALSE` is the setting that was actually wanted. It
strips every `<script>` and `on*` attribute during parsing, so nothing the page
ships ever runs — the whole of the 3.6x frame win — while user scripts and
`evaluate_javascript` keep working. The documentation is explicit: *"executing
JavaScript is still allowed if enable-javascript is TRUE"* (since 2.24).

Measured on device with the two settings, one page carrying an inline
`document.title = "SCRIPT-RAN"`:

| | scroll events | page's own script |
|---|---|---|
| `enable-javascript=FALSE` | 0 | never ran |
| `enable-javascript-markup=FALSE` | 3 | never ran |
| both on | 3 | ran |

The general shape: **a kill switch that is coarser than the feature you meant
to kill takes your own code down with the page's.** Look for the narrower one.

Related: **initial settings must go through the environment**, not the protocol.
`ready` is emitted immediately before the first `load_uri`, so anything sent in
reply arrives after the load has started — which is why the first "JS off" test
was invalid and why the user agent is passed as `N9_USER_AGENT`. A site decides
which layout to serve from the very first request.

## QML

**A page silently fails to open.** A missing QML type resolves at *creation*
time. Use `N9_TEST_QML=1`, which instantiates every page and reports errors; it
caught `InfoBanner` having `timerShowTime` rather than `timeout` before it
shipped. See [qml-components.md](qml-components.md).

**A handler stops halfway with no error.** A ReferenceError in a QML signal
handler abandons the rest of the function silently.

**An off-screen focus target needs `opacity: 0`, not `visible: false`** — an
invisible item cannot take focus.

**Enter inserts a line break instead of submitting the form.** The virtual
keyboard delivers Enter as a committed `"\n"`, not as a key event, so it
arrived on the `text` path — where it was being replaced with a space, silently
changing what was typed. Split on newlines and send `key 65293` instead: a page
can only act on Return if Return actually happens.

**The space bar does nothing in a page, everywhere, while it works in the
address bar.** The address bar is a plain Qt field and never touches the
protocol; a page's text goes over it one keystroke at a time, so a space is sent
alone as the line `text␣␣`. The engine chomped that line with `g_strchomp()`,
which strips *all* trailing whitespace and not just the terminator, leaving
`"text"` — which then matched no command and was dropped without a trace line,
because the trace prints the command it is about to run. Strip the terminator
only. The general shape: **the last field of this protocol is allowed to be a
space, so nothing on the wire may be whitespace-trimmed.**

**The keyboard lags and re-capitalises after every letter.** Both from the same
line: clearing the capture field's `text` after each keystroke. Setting `text`
programmatically calls `updateMicroFocus()`, an IPC round trip to the
input-method server on every letter — and it hands that server an *empty* field,
which is exactly when a keyboard switches auto-capitalisation back on. Forward
the appended substring instead and empty the buffer only on focus out, with
nothing focused to notice.

**A page does not scroll and its last rows are missing.** A bare `Column` on a
`Page` neither flicks nor clips: content taller than the screen simply runs off
the bottom, silently. Wrap it in a `Flickable` with `contentHeight`, `clip:
true` and a `ScrollDecorator`. Keep `Dialog`s (and `SelectionDialog`) **out** of
the Column — a dialog takes its width from its parent and centres itself in it,
so inside scrolling content it becomes another row centred on the wrong thing.

## Tooling

**A failed edit can still produce a passing build.** A QML file written to the
wrong path (`qml/` instead of `shell/qml/`) raised in the edit script, but the
shell still built and the self-test still passed, because the C++ half had
applied — the engine support would have shipped with no UI driving it. Grep for
a marker in the file after editing, do not infer success from a clean build.

**`invoker --single-instance` hands off to an already-running instance.** A
side-by-side test then silently measures the *installed* build. Drop the flag.

**Wait on a condition, not a duration.** `until grep -q "^loading 0" out; do
sleep 1; done` with a cap is both faster and more reliable than a fixed sleep,
which is simultaneously too long for a local file and too short for a heavy page.

## Measurement

**A number that cannot be reproduced is not a result.** Run-to-run variance on
this device is ~16% on CPU for an identical configuration. See
[performance.md](performance.md) for the full list of measurement traps —
compile-time defaults overriding env vars, stdout buffering zeroing frame
counts, unpaced touch events, `pgrep -f` matching nothing, and
`invoker --single-instance` silently testing the installed build instead of the
new one.
