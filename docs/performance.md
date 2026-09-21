# Performance: what was measured, what worked, what did not

Every number here came from the device. The harnesses are in `probe/`. Read the
**Measurement pitfalls** section before trusting any new number — several of the
numbers in this project's history were wrong because of them.

## The shape of the problem

The N9 has **one CPU core**. On a heavy page the browser is CPU-bound, not
memory-bound:

- `free` shows ~120–180 MB available during a github session and **swap stays at
  0** — it is not thrashing.
- `dmesg` contains no OOM kill (the buffer still held boot messages after 10h
  uptime, so it had not wrapped).
- `WPEWebProcess` sits at 70–81% of the core while scrolling.

So the levers that matter are the ones that reduce main-thread work.

## Applied: serialise JIT compilation

`JSC_useConcurrentJIT=false`, set by the engine (`app/n9engine.c`) with
`setenv(..., 0)` so an external override still wins. The web process inherits it.

`probe/jsbench.html`, times in ms, lower is better:

| config | fib27 | loop3M | props | strings | sort | json | **total** |
|---|---|---|---|---|---|---|---|
| default | 189 | 1953 | 281 | 67 | 1165 | 293 | 3948 |
| **useConcurrentJIT=false** | **41** | 1961 | **132** | 67 | **719** | 278 | **3198** |
| useJIT=false | 803 | 3475 | 1072 | 186 | 8213 | 371 | 14120 |
| useDFGJIT=false | 586 | 3590 | 532 | 87 | 3479 | 280 | 8554 |

**Why:** JSC's concurrent DFG compiler runs on a background thread. With one
core it competes with the JavaScript it is trying to accelerate, and the
optimised code lands later. Reproducible across four runs (41, 42, 41, 41).

Also confirmed here: **the JIT is enabled and working** — default is 3.6x
faster than `useJIT=false`. A one-off test with the JIT disabled (while hunting
the shader crash) was an env var for that run only.

## Tested and rejected

| change | result | verdict |
|---|---|---|
| `JSC_useConcurrentGC=false` | 3425 vs 3140 total | **9% worse** — concurrent GC is worth keeping |
| `JSC_thresholdForOptimizeAfterWarmUp=500` | 3202 vs 3140 | no effect |
| both of the above | 3640 | worse |
| `JSC_forceRAMSize` 128M / 256M | see below | **unproven — inside the noise** |

### JSC_forceRAMSize: not proven

The idea was sound: JSC sizes its heap from perceived RAM, the device reports
985 MB, and only ~150 MB is free. But:

| run | default | forceRAMSize=128M |
|---|---|---|
| round 1 | cpu 1408, frames 127 | cpu 1156, frames 134 |
| round 2 | cpu 1181, frames 128 | cpu 1155, frames 102 |

Default's own CPU varied by 16% between rounds and the frame count moved the
wrong way on the repeat. **Not applied.** If revisited, run 5+ rounds and
compare medians; a single A/B cannot resolve an effect this size.

## The dominant cost on a heavy page is main-thread occupancy, not cycles

github, scrolling, JS on vs off — the most informative measurement taken:

| | CPU ticks/22s | frames rendered |
|---|---|---|
| JS on | 789 | 374 |
| JS off | 769 | **1351** |

Nearly identical CPU, **3.6x more frames**. The page's JavaScript is not burning
extra cycles; it is *occupying the main thread* and starving frame production.

This is why JIT tuning did not help github: an A/B there measured 1547 vs 1544
ticks, i.e. nothing. The microbenchmark gain is real, but github's scroll was
never JIT-bound.

**JavaScript therefore defaults to OFF**, and is a switch in Settings. This
reverses an earlier position in this file — that disabling JS was not an
acceptable answer because the modern web needs it. That position was held and
then abandoned on use, not on the numbers: the browser is simply not usable with
scripts on. The measurement explains why no amount of engine work would have
rescued it. The cost is not cycles a faster engine could win back, it is one
core being held by the page, and nothing recovers a 3.6x frame gap.

The trade is real and the user pays it: sites that render nothing without
scripts show nothing until the switch is flipped. The setting persists, so it is
flipped once. Engine-side optimisation is still the only thing that helps a page
that genuinely needs its JavaScript.

## Applied: memory-pressure settings for the WEB process

`app/n9engine.c` used to say WPE 2.44 exposes nothing for the web process and
set `WebKitMemoryPressureSettings` on the network session alone. That was
wrong. `WebKitWebContext` has had a **`memory-pressure-settings` property since
2.34** (`WebKitWebContext.cpp:597`); it is `CONSTRUCT_ONLY`, so the context has
to be built with `g_object_new` rather than `webkit_web_context_new()`. Traced
in the tree, it feeds `configuration.setMemoryPressureHandlerConfiguration()`
and reaches `WebProcessCreationParameters` for every web process the context
creates -- which is what makes WebKit's own `releaseMemory()` run **inside** the
web process: decoded images dropped, a JSC GC, caches shrunk. Nothing in the
shell can reach any of that.

Web process 150 MB, network process 64 MB (was 200, which is above what the
device has free, so it could never be reached); conservative 0.33, strict 0.50,
kill 0.90.

`https://github.com/zsavajji`, medians of 3, live network, `probe/bench-host.sh`:

| | JS | web VmHWM kB | frames | web CPU (scroll) |
|---|---|---|---|---|
| before | off | 76040 | 678 | 755 |
| **after** | off | 76040 | 685 | 799 |
| before | on | 129164 | 471 | 1623 |
| **after** | on | **119004** | 596 | 1672 |

**-10.2 MB (-7.9%) of web-process peak with JavaScript on**, frames and CPU
unchanged. JavaScript off is untouched to the kilobyte, which is the expected
shape: that arm peaks at 76 MB and never approaches the 150 MB limit, so the
handler never fires. The win is only available where the process was actually
growing.

Three runs, not the five this file's own rules ask for. `VmHWM` is the most
reproducible number here -- the JS-off arm returned *identical* 76040 kB before
and after -- so the delta is trusted further than a 3-run CPU delta would be,
but it is not yet a five-run result.

**Do not** set `WEBKIT_DISABLE_MEMORY_PRESSURE_MONITOR=1` to quieten the
UI-process poller. It is the only switch for that monitor, and it also sets
`shouldSuppressMemoryPressureHandler` (`WebProcessPoolGLib.cpp:122`), which
turns off the very handler this change installs.

Not yet tried: kill threshold disabled. Losing the page is the worse outcome on
this device, and 0.90 of 150 MB is reachable.

## Applied: the shell's cache levers now reach the engine (and mostly do not pay)

`BrowserEngine` offered three cache SIZES -- disk MB, memory MB, bfcache
entries. Each set a member, emitted `cacheChanged()` and reached the engine
never, so memory pressure Low did nothing at all and Critical cleared the
**disk** cache, which frees no RAM and costs refetches. WebKit's GLib API has
no cache-size setter, so they could never have worked. They are deleted.

What the API does have is three switches, now protocol commands
(`docs/protocol.md`): `set pagecache <0|1>`, `cachemodel <viewer|browser>`,
`clearmemcache`. Low = pagecache off + clearmemcache; Critical = also cachemodel
viewer; Normal restores.

Engine driven standalone, JavaScript **on**, two navigations first so a
back/forward cache entry exists, `WPEWebProcess` `VmRSS`:

| after | VmRSS kB | delta |
|---|---|---|
| (before shedding) | 111140 | |
| `set pagecache 0` | 108292 | **-2.8 MB** |
| `clearmemcache` | 108296 | +4 kB |
| `cachemodel viewer` | 110684 | **+2.4 MB** |

**Only dropping the back/forward cache pays**, and it pays about 2.8 MB. The
other two show nothing and the cache-model switch measures slightly *worse*.

Two reasons not to read that as "the memory cache is empty", and they matter
before anyone repeats this:

- **RSS cannot see memory returned to the allocator but not to the OS.** Freeing
  a cache inside the web process hands the pages back to bmalloc, which keeps
  them. A null RSS delta is evidence about the metric as much as about the
  cache. `VmHWM` is worse still -- it is a high-water mark and cannot go down
  at all.
- One run, not five.

Kept anyway, because the alternative is not "something better" but the dead
code it replaces: these commands demonstrably arrive (`cache model viewer` /
`cache model browser` traced on device, engine still rendering 72 frames
afterwards, no failures), where the old ones demonstrably did not. The honest
summary is that the shell's own shedding is worth ~3 MB and the real lever is
the web process's own pressure handler above.

Worth trying if this is revisited: whether `cachemodel viewer` earns its place
at all, measured with something that can see inside the allocator rather than
RSS.

## Render scale: user-switchable, effect unmeasured

Settings → Rendering → **Fast** (1.0x, default) / **Sharp** (1.5x). The layout is
320 CSS px wide either way; only rasterisation density changes. Fast rasterises
320x449 and upscales, 2.25x fewer pixels.

Two attempts to A/B this produced junk (see pitfalls). A third showed both
scales pinned at ~81% of a core, which — if real — suggests rasterisation is not
the bottleneck and Fast will help less than the pixel count implies. The toggle
exists so the trade can be judged on the device rather than guessed.

## Measurement pitfalls

Each of these produced a confidently wrong number at least once.

- **The engine's compile-time defaults override the environment.** An early A/B
  of render scale set `N9_LAYOUT_WIDTH`, which only the *backend* reads; the
  engine had it as a `#define` and overrode it. Both arms rendered identically.
  Always print the value the engine actually used and check it differs.
- **stdout is fully buffered when redirected to a file.** Counting `frame` events
  from a redirected engine gave 0 every time. It works when the writer flushes
  per line — verify the count is non-zero before believing a comparison.
- **Touch events need pacing.** Sending `down/motion/motion/up` back-to-back
  produces almost no frames; WPE's gesture controller needs ~1 s between them to
  behave like a real drag. Unpaced runs reported 0 frames.
- **`pgrep -f` patterns silently match nothing** (or the wrong thing), leaving
  `cpu=0`. Resolve the pid once, check it is non-empty, and reuse it.
- **`invoker --single-instance` hands off to an already-running instance**, so a
  side-by-side test can silently measure the *installed* build instead of the
  new one. Drop the flag when testing.
- **A locked or blanked screen suppresses frame production.** The same page
  (`tall.html`, JS off, identical command) scored **9-11 frames locked and 55
  unlocked** -- a 5x error, dwarfing every effect being measured, and it looks
  exactly like a slow page. CPU and RSS were unaffected, so nothing else in the
  run hints at it. `probe/bench.sh` asks MCE for
  `get_display_status`/`get_tklock_mode` before and after every run, reports
  them as `display=`, and warns when they are not `on:unlocked`. It also holds
  the display on (`req_display_blanking_pause`, renewed every 30 s, because the
  pause lapses after about a minute). It **cannot** unlock the phone: `user`
  (uid 29999) is refused `req_tklock_mode_change` by D-Bus policy, so a locked
  device must still be unlocked by hand.
- **Run-to-run variance is large** (~16% on CPU for an identical config). Treat
  anything under ~20% as unproven without repeats.

## Harnesses

| tool | what it measures |
|---|---|
| `probe/jsbench.html` | JS execution: calls, numeric loops, property access, strings, sort, JSON. Reports via `document.title`, which the engine emits as a `title` event — so results come out over the protocol with no instrumentation. |
| `probe/glcompile.c` | Replays a captured shader against the driver. |
| `probe/gllimits.c` | GL limits. |
| frames + CPU | Drive the engine standalone over stdin, count `frame` lines in its stdout, read `utime+stime` from `/proc/<pid>/stat`. |

Example JS benchmark run:

```sh
( sleep 22; echo quit ) | env JSC_useConcurrentJIT=false LD_LIBRARY_PATH=/opt/n9browser/lib \
  WPE_BACKEND=/opt/n9browser/lib/libWPEBackend-n9.so DISPLAY=:0 \
  /opt/n9browser/bin/webmee-engine file:///home/user/jsbench.html \
  2>/dev/null | grep -m1 "^title BENCH"
```

## Ideas not yet tried

- Font rasterisation cost: `deps/compat/fonts/fonts.conf` is ours, so hinting and
  antialiasing settings are a lever on glyph rendering.
- WebKit tile size / layer limits in the TextureMapper.
- `libsoup` connection limits — fewer parallel connections may reduce churn on a
  slow link.
- Bytecode caching across loads, if WPE exposes it.
- Reducing full-viewport Qt repaints in the shell (currently `FullViewportUpdate`,
  required by the GL viewport).
