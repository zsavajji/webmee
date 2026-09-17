# Process-swap-on-navigation: the browser stalls every few links

**Status: fixed in `deps/build-webkit.sh` (`patch_disable_process_swap`).**
Everything cheaper was tried and ruled out on the device first; this file records
what, so none of it is repeated.

## The symptom

After roughly three cross-site navigations the browser pauses for tens of
seconds, and a navigation can be lost outright — the address bar shows the new
url, the old page stays on screen, and nothing further happens until a web
process is reaped.

It surfaced as "tabs are broken", because switching tabs is a navigation and so
hits it every single time. It is **not** a tab problem: plain `load` reproduces
it exactly, with no tabs involved.

## The measurement

Engine driven standalone over stdin (the harness pattern from
[performance.md](performance.md)), five loads alternating between two hosts,
9 s apart, JavaScript off, page area 480×674:

```
t=0s    webprocs=1  memfree=101396kB  loads_done=0
t=10s   webprocs=2  memfree= 89084kB  loads_done=1
t=20s   webprocs=4  memfree= 64416kB  loads_done=2
t=30s   webprocs=4  memfree= 58612kB  loads_done=3
t=40s   webprocs=4  memfree= 60432kB  loads_done=3     <- stalled
t=50s   webprocs=4  memfree= 61488kB  loads_done=3
t=60s   webprocs=4  memfree= 61364kB  loads_done=3
t=70s   webprocs=3  memfree= 73620kB  loads_done=4     <- one reaped, one load through
t=100s  webprocs=1  memfree=107308kB  loads_done=4
```

**Five loads sent, four completed.** One was lost and never retried. stderr
carried `Error sending IPC message: Bad file descriptor` twice — the UI process
talking to a connection that no longer exists.

Reproduced identically on a second run (99172 → 62572 → … → 105660 kB, same
stall, same four-of-five). Well outside this device's ~16% run-to-run variance.

Note the pages here are `example.com` and `example.org` — as light as the web
gets. Real sites are heavier, so this is a **floor**, not a typical case.

## The cause

WebKit starts a fresh web process for every cross-site navigation and keeps the
old ones alive for a while. On a device with ~100 MB free and a
`WPEWebProcess` that peaks at 116–127 MB for a single page, four at once is
over budget, and the kernel arbitrates by making everything wait.

Two things it is **not**, both checked in the source:

- **Not the web process cache.** `WebProcessCache::updateCapacity` does
  `if (memorySize < 3) m_capacity = 0;` against `ramSize() / GB`, which is 0
  here. The cache disables itself on this device already. These are live
  processes.
- **Not process prewarming on its own.** Prewarming is gated on the same flag
  (`WebProcessPool.cpp:1364`), so it goes away with the swap and cannot be
  turned off separately.

The switch is `page->preferences().processSwapOnCrossSiteNavigationEnabled()`,
read in `WebProcessPool.cpp:1272` and pushed into the pool configuration.

## Why it cannot be turned off at runtime

Three routes, all closed. Verified, not assumed:

1. **The GLib settings API.** `webkit_settings_set_feature_enabled()` exists and
   looks like the answer. It is not. `webkit_settings_get_all_features()` only
   exposes preferences with a WebCore binding, and this one is declared
   `webcoreBinding: none, exposed: [ WebKit ]`
   (`UnifiedWebPreferences.yaml:5334`, status `stable`, default `true`).
   Tried on device — the engine enumerated every feature and printed
   `process swap on navigation: NOT FOUND`, with the process count unchanged.

   The identifier string *is* in the shipped library, which makes this look
   workable from a `strings` grep alone. It is in there as a preference, not as
   a feature.

2. **The WebKit C API.** `WKContextSetUsesSingleWebProcess` and
   `WKContextConfigurationSetProcessSwapsOnNavigation` would both do it, and the
   backend is already reached by `dlsym`, so the same trick would serve. But
   `nm -D` on `libWPEWebKit-2.0.so` finds **zero** `WKContext` exports — the C
   API is not built into this library. There is nothing to resolve.

3. **The environment.** No env var reaches this configuration in 2.44.

## The fix

`patch_disable_process_swap` in `deps/build-webkit.sh`, alongside the
rounded-rect patch. One line in **`WebKitWebContext.cpp`**:

```cpp
configuration.setProcessSwapsOnNavigation(false);   // WPE branch of the #if
```

**Not `WebProcessPool.cpp`, which is the obvious-looking site and does
nothing.** The value is

```cpp
m_processSwapsOnNavigationFromClient.value_or(m_processSwapsOnNavigationFromExperimentalFeatures)
```

and `WebProcessPool.cpp` writes the *experimental* half from the page
preference, while the GLib context writes the **client** half — which wins.
Patching the pool builds, installs, and changes the process count not at all;
that was tried first and measured. GTK reads a property there, WPE hardcodes
`true`, so WPE takes the `#else` branch.

Everything else follows. The web process cache is already disabled below 3 GB of
RAM, and automatic process prewarming is gated on the same flag
(`WebProcessPool.cpp:1370`), so it goes away too.

### Measured after the patch

Same probe, same device:

```
t=0s    webprocs=1  memfree=261608kB  loads_done=0
t=10s   webprocs=2  memfree=180848kB  loads_done=1
t=50s   webprocs=2  memfree=180244kB  loads_done=4
t=100s  webprocs=2  memfree=179988kB  loads_done=6
```

Flat at two processes instead of climbing to four, memory steady instead of
falling to 58 MB, **every** load completed instead of four of five, and no
`Error sending IPC message: Bad file descriptor`.

**Verify the artifact, not the source.** `deploy.sh` strips the libraries, so
the shipped `libWPEWebKit` never matches the build tree's md5 and "the patch did
not ship" is the wrong conclusion to draw from that. Compare against a locally
stripped copy.

**Cost: about four minutes, not two hours.** `WebProcessPool.cpp` sits in a
unified-source bundle, so exactly one TU recompiles, and the rest is relinking
`libWPEWebKit`, the two auxiliary processes and the inspector resources —
**18 edges**. This estimate was originally written as "expect a large rebuild";
that was wrong, and wrong in the direction that stops you attempting the fix.

Beware `ninja -n | wc -l` as a cost estimate: it counts output LINES, and a
command spans several. The run itself reports `[1/522]`, then `[3/34]`, then
`[6/18]` — the denominator collapses as `restat` generators produce identical
output and ninja prunes the subtree it had conservatively marked dirty. Read the
final denominator, not the dry run.

**What it costs the user:** process-swap exists to isolate sites from each
other, so one process now renders every site. On a single-user phone browser
with JavaScript off by default, the memory is worth more than the isolation —
but it is a real security property, not free, and this is a deliberate decision
rather than a side effect.

## Reproducing it

On the device, with the engine either installed or dropped in `/home/user`:

```sh
OUT=/home/user/ps.out; ERR=/home/user/ps.err; rm -f "$OUT" "$ERR"
(
  echo "geometry 0 0 480 674"; sleep 6
  i=1
  while [ $i -le 5 ]; do
    if [ $((i % 2)) -eq 1 ]; then echo "load http://example.com"
    else echo "load http://example.org"; fi
    sleep 9; i=$((i + 1))
  done
  echo quit
) | env LD_LIBRARY_PATH=/opt/n9browser/lib \
        WPE_BACKEND=/opt/n9browser/lib/libWPEBackend-n9.so \
        DISPLAY=:0 N9_NO_JS=1 \
        /opt/n9browser/bin/webmee-engine http://example.org > "$OUT" 2>"$ERR" &
i=0
while [ $i -lt 11 ]; do
  echo "t=${i}0s webprocs=$(ps | grep -c '[W]PEWebProcess')" \
       "memfree=$(awk '/^MemFree/ {print $2}' /proc/meminfo)kB" \
       "loads_done=$(grep -c '^loading 0' "$OUT")"
  i=$((i + 1)); sleep 5
done
```

Five loads in, fewer than five `loading 0` out is the failure. Count the
processes rather than trusting the clock: the stall length depends on what else
the phone is doing.

Run it before and after the patch: before, the process count reaches four and a
navigation is lost; after, it should stay at one.

Two traps worth keeping (both cost time here):

- **Do not pipe the engine's stdout through `grep`.** grep buffers when its
  output is not a terminal and the tail of the run is lost, which reads as "the
  engine stopped emitting". Write stdout to a file — the engine flushes per
  line — and filter afterwards.
- **`grep -c <string> <binary>` on the host silently matches nothing.** Use
  `strings -a | grep -c`. A "the symbol is not there" conclusion drawn the first
  way is wrong, and it was drawn that way here before being corrected.
