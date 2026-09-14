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

**A GL viewport needs `FullViewportUpdate`**; Qt cannot track dirty regions
across the swap.

**`InfoBanner is not a type`** — needs `import com.nokia.extras 1.1` (1.0 is not
enough).

**A page silently fails to open, with nothing in the log.** A QML type that does
not exist resolves at *creation* time, not load time. `SelectionListItem` is not
a Harmattan component; `SelectionDialog` is. Check
[qml-components.md](qml-components.md), and use `N9_TEST_QML=1` to instantiate
every page at startup instead of waiting for a tap.

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

**Unexplained web-process crashes after a deploy.** The injected bundle shipped
to the wrong path because a `cp` flattened `lib/wpe-webkit-2.0/`. WebKit
`dlopen`s it by absolute path, and a missing bundle is not reported as a load
error. `deploy.sh` asserts the critical paths now.

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
