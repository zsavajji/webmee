# Debugging on this device

There is no gdb and `core_pattern` is `/dev/null`. Every post-mortem has to come
from inside the process. These tools exist because of that.

## probe/n9crash.c — crash reporter and GL tracer (LD_PRELOAD)

```sh
LD_PRELOAD=/home/user/n9crash.so <app>
```

On SIGSEGV/SIGBUS/SIGABRT/SIGILL/SIGFPE it prints the signal, faulting address,
program counter, a backtrace, and `/proc/self/maps`, then re-raises so the
process still dies normally. Resolve the PC against the maps to name the
library:

```python
lo <= pc < hi  ->  module, file_offset = map_offset + (pc - lo)
```

`LD_PRELOAD` is inherited by the web process WebKit spawns, which is the point:
it reaches code you cannot otherwise instrument, without root.

It also interposes `glShaderSource`/`glCompileShader`/`glAttachShader`/
`glLinkProgram` to capture shader sources, and dumps the shader it died in.

| variable | effect |
|---|---|
| `N9_LOG_SHADERS=1` | print every shader as it is compiled |
| `N9_SHADER_DUMP=<dir>` | write each shader to `<dir>/shader-<id>.glsl` |
| `N9_LOG_DLSYM=1` | log GL symbol resolution requests |

### Four traps this tool had to work around

Each of these produced a confident wrong answer first.

1. **Ordinary symbol interposition does not work.** WebKit reaches GL through
   **libepoxy**, which `dlsym`s entry points off a `dlopen`ed handle and bypasses
   the PLT. You must interpose `dlsym` itself (and `eglGetProcAddress`).

2. **`RTLD_NEXT` finds nothing.** epoxy opens libGLESv2 with `RTLD_LOCAL`, so it
   is not in the global scope. A wrapper that resolves its real function with
   `RTLD_NEXT` gets NULL and — if it then returns early — silently turns the call
   into a **no-op**. That "fixed" the crash by never compiling a shader, which
   looked like a real finding for a while. Resolve through the handle epoxy
   passed to `dlsym`.

3. **`dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0")` returns NULL here** — this ARM
   EABI glibc versions it `GLIBC_2.4`. When that fails and the interposer returns
   NULL for every symbol, unrelated things break in mystifying ways: libsoup3's
   check for libsoup2 symbols misfires and aborts with "libsoup2 symbols
   detected".

4. **Two crash handlers fight.** The WPE backend installs its own and is loaded
   *after* the preload, so its `sigaction` wins — and it cannot dump shaders.
   The preload re-arms itself at the first GL call.

Also: **shader ids here are large** (e.g. 350005), so any table indexed by id
captures nothing. Use a keyed table.

## probe/glcompile.c — replay a shader against the driver

```sh
DISPLAY=:0 ./n9glcompile shader.glsl; echo $?     # 0 ok, 1 compile error, 139 SIGSEGV
```

Compiles one fragment shader from a file and reports the result. This turns a
20-minute WebKit rebuild into a two-second experiment: capture a shader with
`N9_SHADER_DUMP`, edit it, replay it. It is how the rounded-rect diagnosis was
settled after the first hypothesis (uniform budget) proved wrong.

Run each variant as its own process — a crash is a valid result.

## probe/gllimits.c — GL limits

Prints the uniform/varying/texture limits. Note it creates a real 16×16 X
window: this driver offers **no pbuffer config at all** and lacks
`EGL_KHR_surfaceless_context`, so there is no other way to make a context
current.

## probe/tfpprobe.c — texture-from-pixmap canary

Walks the whole compositing path (redirect → name pixmap → EGLImage → texture →
draw → read back) and checks the two properties the design relies on: that the
texture tracks the window without rebinding, and which way up the pixmap is.

Orientation cannot be established with a flat colour — paint a scissored split.

## In-process pixel verification

```sh
N9_VERIFY_PAGE=3 <app>
```

After drawing, the shell `glReadPixels` its own framebuffer and logs the values.
This distinguishes "not composited" from "the screen is off or the phone is
locked", which a screenshot cannot.

**Sample only after the first `frame` event.** Every paint during startup
precedes it and reports black — which reads as a failure and is not one.

## Screenshots

```sh
ssh user@n9 'DISPLAY=:0 /home/user/n9shot' > shot.ppm
ffmpeg -i shot.ppm -vf transpose=1 shot.png
```

`n9shot` writes a PPM to **stdout**; it does not take a filename. If the phone
is locked you get the lock screen, not the app, and mostly-black output is the
tell.

## Method notes

- **"no crash" is not "it worked".** Check pixels or a title, not just the
  absence of a failure. An `example.com` test that only proved "did not crash"
  left the TLS stack unverified for hours.
- **`frame` goes down the pipe to the shell, not into the log.** Counting it in
  the log always yields zero.
- **Grep a binary instead of `strings`** — busybox has no `strings`:
  `grep -c "ROUNDED_RECT_ARRAY_SIZE 12" lib.so`.
- **Verify the artifact, not the source.** Check the shipped `.so` contains the
  patch before concluding a fix did not work.
- **Bisect with real pages.** Saving a page locally, stripping scripts, and
  halving the DOM turned "github crashes" into a seven-line reproducer with no
  network and no JS.
