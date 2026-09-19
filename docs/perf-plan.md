# RAM and performance plan

A brief for an agent. Goal: lower WebMee's memory footprint and raise its
frame rate and load speed on the N9, with JavaScript off (the default) and
with JavaScript on. Every change is measured before it is kept.

## Read first

`docs/performance.md`, `docs/gotchas.md`, `docs/process-swap.md`,
`docs/building.md`, `docs/debugging.md`. They record every measurement taken
so far and every trap that produced a wrong number. Do not repeat a rejected
experiment (concurrent GC off, tier-up threshold, per-tab session state) unless
this plan says why.

## Environment

- Builds run on the Linux host that has `~/cross/arm-harmattan`, `~/QtSDK/Madde`
  and this repo's `build/`, `build-wpe/`, `out/` trees. They are not on the
  macOS machine.
- The phone is `ssh user@n9`. It is a development device in open mode; deploying
  and killing processes on it is expected. Installing a `.deb` needs root, which
  only the user can do. Plan the work so that engine changes iterate with
  `N9_ENGINE=/home/user/webmee-engine` and WebKit or backend changes are batched
  into as few installs as possible.
- `deps/build-webkit.sh` only configures; the compile is `ninja -C build-wpe`
  with `-j5`. A change to `LDFLAGS` wipes the build directory. A one-file WebKit
  change rebuilds in minutes; read the final ninja denominator, not `ninja -n`.

## Rules

1. **No number without the harness** (Phase 0). Variance is ~16 % run to run.
   Five runs, medians, JavaScript on and off, reported as a table in
   `docs/performance.md`. A change that cannot be shown to help is reverted.
2. **Verify the artifact.** After a build, confirm the patch or flag is in the
   shipped `.so` or binary (`strings -a | grep -c` on the host, `grep -c` on the
   device), not in the source tree.
3. **WebKit changes are patches in `deps/build-webkit.sh`**, shaped like the three
   that exist: a Python block, idempotent on a marker string, aborting when the
   patch site is not found. Prefer a cmake option to a patch. No fork of WebKit.
4. **A setting is three things**: a write, a read, and delivery to the engine
   (`docs/gotchas.md`, "Settings that look wired but are not"). Anything added to
   `BrowserEngine` must be shown to arrive in `app/n9engine.c`, by a trace line.
5. **Record the measurement, not the belief.** Every step below ends with a
   table row. Conclusions that were reached without one are listed in the
   "claims to verify" section and are to be checked in the tree first.

## Claims to verify before acting on them

These came from reading the code and from memory of the WebKit API, not from
the tree. Each is a grep away.

| claim | check |
|---|---|
| `WebKitWebContext` has a construct-only `memory-pressure-settings` property in 2.44 (the engine comment says the web process has no equivalent) | `grep -n "memory-pressure-settings" build/webkit*/Source/WebKit/UIProcess/API/glib/WebKitWebContext.cpp` |
| WebKit keeps 2 documents in the page cache at this RAM size under `DOCUMENT_BROWSER` | `calculateMemoryCacheSizes` in `Source/WebCore/platform/...` (grep the name) |
| `TiledBackingStore` has a cover-area multiplier ~2.0 and keep-area ~3.0 | `grep -rn "coverAreaMultiplier\|keepAreaMultiplier" Source/WebCore/platform/graphics/texmap/` |
| Content extensions are compiled in | `grep ENABLE_CONTENT_EXTENSIONS build-wpe/cmakeconfig.h` |
| A registered service worker spawns a second `WPEWebProcess` | count processes on GitHub with `N9_NO_JS=0` |
| `AsyncFrameScrollingEnabled` and timer-throttling features are exposed by `webkit_settings_get_all_features()` | dump the list on device (Phase 2.2) |
| `WTF::ramSize()` lives in `Source/WTF/wtf/RAMSize.cpp` and everything RAM-sized reads it | `grep -rn "ramSize()" Source/WTF Source/JavaScriptCore/runtime Source/WebCore/platform \| wc -l` |

## Phase 0: the harness

Deliverable: `probe/bench.sh` (runs on the device) and `probe/bench-host.sh`
(drives it over ssh, collects medians), plus a baseline table in
`docs/performance.md`.

1. **Corpus** at `/home/user/bench/`, saved complete with assets so the network
   is out of the measurement: `example.com`; a Wikipedia article (text and
   fonts); a GitHub repository page (deep DOM, nested rounded clips); a news
   front page (images); one single-page app that renders nothing without
   scripts. Keep the originals' URLs in a README next to them.
2. **Per run**, engine driven standalone over stdin as in
   `docs/process-swap.md`, one page, one JavaScript mode:
   - milliseconds from `load` to `loading 0`
   - `frame` lines during a scripted scroll: `touch down`, ten `touch motion`
     one second apart, `touch up`
   - `utime+stime` from `/proc/<pid>/stat` for the engine, `WPEWebProcess` and
     `WPENetworkProcess`; add the shell when running through the app
   - `VmHWM` and `VmRSS` from `/proc/<pid>/status` for each, at the end
   - `MemFree` and `SwapFree` delta from `/proc/meminfo`; `WPEWebProcess` count
3. **Five runs per cell**, medians. Cells: 5 pages × 2 JS modes × each
   configuration under test.
4. **Pitfalls** (all from `docs/performance.md`, all cost time once): write the
   engine's stdout to a file, never through `grep`; resolve pids once and check
   they are non-empty; pace touches; never `invoker --single-instance` when
   comparing builds; print the value the engine actually used for any knob.
5. **Also once**: idle and peak RSS of the shell and of the network process,
   through the app. Neither has ever been measured, and the split decides
   whether Phase 4 exists.

Acceptance: the baseline table exists, the harness reproduces the documented
JS on/off frame gap (about 3.6×) on the GitHub page.

## Phase 1: RAM, no rebuild

### 1.1 Web-process memory pressure settings

`app/n9engine.c` sets `WebKitMemoryPressureSettings` on the network session
only and states that 2.44 has nothing for the web process. If the
`memory-pressure-settings` property exists (see claims), build the context with
it: `g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", s, NULL)`.
Start at limit 150 MB, conservative 0.33, strict 0.50, kill 0.90; then try kill
disabled, because losing the page is the worse outcome on this device. This
makes WebKit's own `releaseMemory` run inside the web process (decoded images,
JSC GC, cache shrink), which nothing in the shell can reach.

Also lower the network-process limit from 200 MB to about 64 MB; 200 MB is
above what the device has free.

Check what WebKit's UI-process `MemoryPressureMonitor` was doing before
(`Source/WebKit/UIProcess/linux/MemoryPressureMonitor.cpp`): it polls
`/proc/meminfo`, this kernel has no `MemAvailable`, and a monitor that misfires
every second would itself cost CPU. Explicit settings disable it.

Metric: `VmHWM` of `WPEWebProcess` on the news and GitHub pages, frames unchanged.

### 1.2 Replace the dead shell levers

`BrowserEngine::setPageCacheEntries`, `setMemoryCacheMb` and `setDiskCacheMb`
update a member and emit a signal; nothing reaches the engine. So
`onMemoryPressure` level 1 does nothing, and level 2 clears the disk cache,
which frees no RAM and costs refetches.

WebKit's GLib API exposes exactly three runtime controls:
`webkit_settings_set_enable_page_cache`, `webkit_web_context_set_cache_model`
(`DOCUMENT_VIEWER` disables the memory cache) and
`webkit_website_data_manager_clear(..., WEBKIT_WEBSITE_DATA_MEMORY_CACHE, ...)`.

- Add protocol commands `set pagecache <0|1>`, `cachemodel <viewer|browser>`,
  `clearmemcache`; document them in `docs/protocol.md`.
- Delete the three size setters and their properties; keep `cacheUsageMb`.
- `onMemoryPressure`: Low = pagecache 0 + clearmemcache; Critical = also
  cachemodel viewer; Normal = restore both. Stop sending `clearcache` on
  pressure.
- `SettingsPage.qml` still shows "Cache: N MB of 64 MB"; the 64 is now a fiction.
  Show usage only.

Metric: `VmRSS` of `WPEWebProcess` before and after a forced Low and Critical
(lower `memory.setThresholds` temporarily to trigger them).

### 1.3 Page cache default

If WebKit keeps two documents in bfcache here, measure `VmHWM` across the
corpus with `enable-page-cache` off. If it saves more than about 10 MB, default
it off and let Back refetch; the disk cache is on eMMC.

### 1.4 Process count with JavaScript on

Load the GitHub page with `N9_NO_JS=0`, count `WPEWebProcess`. If a service
worker spawns a second one, that is Phase 3.1's first flag; note the RSS it
costs.

## Phase 2: CPU, no rebuild

### 2.1 Content blocker

The largest lever with JavaScript on, and a real one with it off (iframes,
tracking pixels, third-party images still load). API: `WebKitUserContentFilterStore`
and `webkit_user_content_manager_add_filter`, present since 2.24. Ship a
compiled EasyList + EasyPrivacy subset in Safari content-blocker JSON, compile
once at first run into `~/.cache/WebMee/filters/`, add to the existing
`WebKitUserContentManager`. Keep the ruleset small enough that compiling it on
a 1 GHz core is a one-time few seconds; log the time.

Measure with the network ON, as a separate run set: this change is about the
real web, not the offline corpus. Metrics: load time, frames, `VmHWM`, request
count if the network process logs it.

### 2.2 Feature knobs

The engine already knows how to enumerate `webkit_settings_get_all_features()`
(it did so for the process-swap flag). Dump the list on device to stderr once,
commit it as `docs/features.txt`, then A/B only what is present, one line each
with `webkit_settings_set_feature_enabled`:

- `AsyncFrameScrollingEnabled`: if present and off, the compositor thread scrolls
  already-rendered tiles while JavaScript holds the main thread. This targets
  the documented JS-on frame gap directly. Highest expected value.
- `DOMTimersThrottlingEnabled`, `RequestAnimationFrameEnabled`,
  `WebAnimationsEnabled`, IntersectionObserver: fewer wakeups with JS on.

A string being in the binary is not a feature; only the enumerated list counts.

### 2.3 JSC options

Via `setenv(..., 0)` in `n9engine.c` so an external override still wins. Re-run
`JSC_forceRAMSize=256M` against `VmHWM`, not frames: the earlier A/B measured
the wrong metric for a heap-sizing knob. Leave the rejected options rejected.

### 2.4 Per-site JavaScript allowlist (shell, optional)

JavaScript stays off. A `mainMenu` item "Run scripts on this site" persists the
host in `settings.conf`; `BrowserEngine::load` sends `set javascript 1` before
`load` when the host matches and `set javascript 0` otherwise. The setting is
global per view, so it must precede the load. Not a performance change; it is
what makes "JS on" affordable per site. Cover it in `test/host/tabtest.cpp`
style with a stub engine.

### 2.5 Fonts

`deps/compat/fonts/fonts.conf`: `hintslight`, `rgba none` (no subpixel),
embedded bitmaps allowed. Measure on the Wikipedia page, JS off.

### 2.6 Render scale

Re-run Fast (1.0) vs Sharp (1.5) with the harness. The current "not the
bottleneck" note in `docs/performance.md` rests on two junk runs and one
unrepeated one.

## Phase 3: WebKit build and source (rebuild)

Prefer cmake options. Each item is one change and one table row.

### 3.1 Build options

In `deps/build-webkit.sh`: `-DENABLE_SERVICE_WORKER=OFF` (the extra web
process), `-DENABLE_REMOTE_INSPECTOR=OFF` (inspector resources and code),
`-DENABLE_NOTIFICATIONS=OFF`, `-DENABLE_GEOLOCATION=OFF`,
`-DENABLE_OFFSCREEN_CANVAS=OFF`; confirm `ENABLE_GPU_PROCESS` is off.
`-DENABLE_INDEXED_DATABASE=OFF` is optional and riskier for JS-on sites.
Metrics: `libWPEWebKit-2.0.so` stripped size, idle `VmRSS` of the web process,
process count on GitHub with JS on.

### 3.2 `WTF::ramSize()`

Patch `Source/WTF/wtf/RAMSize.cpp` so the Linux branch honours
`WEBKIT_RAM_SIZE_MB` from the environment, and pass `WEBKIT_RAM_SIZE_MB=256`
from `BrowserEngine::startEngine`. Everything that sizes itself off RAM
follows: JSC heap growth, memory cache size, page-cache count,
memory-kill thresholds. This is the source-level `forceRAMSize` and reaches
WebCore too. One translation unit. Metrics: `VmHWM`, frames, load time.

### 3.3 Tile cover and keep area

`TiledBackingStore` pre-renders tiles around the viewport. On one core every
tile the user never sees is wasted CPU, and tiles are CPU bitmaps until
uploaded. Try cover 1.5 and keep 2.0 (verify names first). Metrics: frames
during the scripted scroll (checkerboarding shows up as a frame that is
rendered but blank; also take a screenshot mid-scroll), `VmHWM`.

### 3.4 Tile size

If the default tile is 512 px, try 256: at Fast the surface is 345 px wide, so
half of every 512 tile is off-surface. Same metrics as 3.3.

### 3.5 Do not

- Move to WebKit 2.46 or later (Skia; `docs/device.md` on the GLSL compiler).
- Add a second web view or per-tab session state (`docs/protocol.md`).
- Disable the threaded compositor; it is why JS-off scrolling is 60 fps.
- Consider Chromium (`docs/architecture.md`).

## Phase 4: shell (conditional)

Only if Phase 0's CPU split shows the shell above about 15 % of the core
during a scroll. The cost would be the `FullViewportUpdate` repaint of the
whole QML scene once per engine `frame`. A GL viewport cannot do partial
updates, so cap `WebViewItem::frameReady()` repaints to the display rate
instead of one per frame event. Nothing else in the shell is on the hot path.

## Order

1. Phase 0. Everything else is guesswork without it.
2. 1.1 and 1.2. Small diffs, the first real control over RAM.
3. 2.1, then 2.2 (async scrolling first). The largest JS-on gains, no rebuild.
4. 3.1 and 3.2 in one rebuild and one install.
5. 3.3 and 3.4 only if Phase 0 shows rasterisation or tile memory in the profile.
6. 2.4, 2.5, 2.6 fill gaps between installs; they need no root.

## Deliverables

- `probe/bench.sh`, `probe/bench-host.sh`, the corpus README.
- `docs/performance.md` extended with one table per change, kept or reverted,
  in the same symptom-first style as the rest of the docs.
- `docs/protocol.md` updated for any new command.
- One commit per measured change, message stating the metric and the delta.
- A closing note in `docs/performance.md` listing what was tried and rejected,
  so the next agent does not repeat it.

## On improving the WebKit source

Yes, narrowly. The port is pinned to 2.44.4 for good, so patches never need
rebasing, and the patch mechanism is proven and cheap. But the biggest gains
available are configuration (memory-pressure settings, the content blocker,
feature flags) and shell plumbing, none of which need a rebuild. Source work
follows the measurements; it does not lead them.
