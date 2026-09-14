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

**Disabling JS is not an acceptable answer** — JS is a large part of the modern
web, and the browser has to run it. The toggle stays in Settings as a user
choice, not as a performance strategy. Optimisation has to come from the engine.

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
