# The device, as measured

Everything here was measured on the target with a probe, not read from a
datasheet. Where a probe produced it, the probe is named. **Do not assume
anything about this device that is not in this file** — several "obvious"
assumptions turned out to be false and cost hours.

## Hardware and OS

| | |
|---|---|
| Device | Nokia N9 (RM696), MeeGo 1.2 Harmattan |
| Kernel | 2.6.32.54 |
| libc | glibc 2.10.1 (2009) |
| Compiler ABI on device | GCC 4.4, Qt 4.7.4 |
| RAM | 985 MB total, typically ~120–350 MB free |
| Swap | 255 MB **ramzswap** — compressed RAM, so spilling costs CPU, not capacity |
| Screen | 854×480 physical, landscape framebuffer |
| GPU | PowerVR SGX530, OpenGL ES 2.0 only |
| Root filesystem | ~3.9 G, ~2.4 G free on `/` (where `/opt` lives) |

Portrait is produced by **Qt rotating its scene inside a landscape X window**.
The framebuffer is always 854×480. This single fact drives the entire
compositing design — see [architecture.md](architecture.md).

## GL/EGL capability profile

Measured by `probe/gllimits.c` and `probe/tfpprobe.c`.

```
EGL              1.4, vendor "Imagination Technologies"
GL_VERSION       OpenGL ES 2.0
GL_RENDERER      PowerVR SGX 530

MAX_TEXTURE_SIZE              2048     <- caps pinch magnification at 3x
MAX_FRAGMENT_UNIFORM_VECTORS  64       <- WebKit's clip shader wants 70
MAX_VERTEX_UNIFORM_VECTORS    128
MAX_VARYING_VECTORS           8
MAX_TEXTURE_IMAGE_UNITS       8
MAX_VERTEX_ATTRIBS            8
```

**Present:** `EGL_KHR_image_base`, `EGL_KHR_image_pixmap`, `EGL_KHR_fence_sync`,
`GL_OES_EGL_image`, `GL_EXT_texture_format_BGRA8888` (Cairo ARGB32 maps straight
to it, no swizzle), `GL_OES_rgb8_rgba8`.

**Absent, and the cost of each:**

- `GL_OES_texture_npot` — not a blocker. Core GLES2 allows NPOT textures with
  `CLAMP_TO_EDGE` and no mipmaps; the extension only lifts those restrictions.
  Our texture code sets both unconditionally for this reason.
- `EGL_KHR_surfaceless_context` — a context cannot be made current without a
  surface.
- `GL_OES_packed_depth_stencil` — only affects WebGL/canvas depth+stencil, and
  WebGL is off.

**There is no pbuffer config at all.** `eglChooseConfig` with
`EGL_PBUFFER_BIT` returns zero configs (not an error — `EGL_SUCCESS` with
`num_config = 0`). Combined with the missing surfaceless extension, *any* code
that needs a current context must create a real X window, even a 16×16 one.
Both `gllimits.c` and `glcompile.c` do exactly that.

### The EGL/X11 visual trap

`eglCreateWindowSurface` returns `EGL_BAD_ALLOC` (0x3005) unless the X window
was created with the visual named by the config's `EGL_NATIVE_VISUAL_ID`
(here id 75, depth 32). You must use `XGetVisualInfo` + `XCreateColormap` +
`XCreateWindow`; `XCreateSimpleWindow` cannot work.

### The GLSL compiler crashes on dynamic uniform array indexing

`libglslcompiler_r125.so` **segfaults** (null deref inside
`GLSLCompileToUniflex`) when a shader indexes a uniform array with a loop
variable. It does not report a compile error — it takes the process down.

Proven by replaying one captured shader in four variants through
`probe/glcompile.c`:

| variant | result |
|---|---|
| no rounded-rect clip | compiled OK |
| original, `array[rectIndex]` in a loop | **SIGSEGV** |
| unrolled, constant indices, 4 rects | compiled OK |
| single rect, no loop | compiled OK |

WebKit's `applyRoundedRectClip` did exactly the crashing thing, so **any page
with two or more nested rounded-corner clips killed the web process** — which
is most of the modern web. The fix (`deps/build-webkit.sh`,
`patch_rounded_rect_limit`) unrolls the loop to constant indices.

Treat this as a general rule for this GPU: **no dynamic indexing of uniform
arrays in any shader.**

## X server

- `XComposite` 0.4 and `XDamage` 1.1 are present; `libXcomposite`/`libXdamage`
  are on the device and their headers are in the Madde sysroot.
- MCompositor redirects **top-level** windows. A child window is not redirected
  by it, so we can redirect one ourselves.
- A redirected window renders to its pixmap at full size regardless of parent
  clipping or position — so the engine's window position is irrelevant.
- `XCompositeRedirectWindow` twice **from the same client** is `BadAccess`, and
  a window has exactly one redirecting client. Only one side may redirect.

## Userland quirks

- **busybox**: no `strings`, no `timeout`, `head` has no `-c`. Grepping a binary
  with `grep -c "literal" file` works and is the substitute for `strings`.
- **`core_pattern` is `/dev/null`** and there is no gdb. Post-mortems must come
  from inside the process — see [debugging.md](debugging.md).
- **`dlsym` is versioned `GLIBC_2.4`** on this ARM EABI build, not `GLIBC_2.0`.
  `dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0")` returns NULL here.
- **`/syspart` memory cgroup is unlimited** (`memory.limit_in_bytes` = 0), so
  there is no per-app memory cap to blame for a kill.
- **applauncherd gives the app no `DISPLAY`.** Qt reaches X through the booster,
  but a `QProcess` child gets nothing and dies. Pass it explicitly.
- **The status bar renders black-on-black** unless the app is launched through
  `invoker` — applauncherd is what registers the window with the compositor.

## Privilege

| | |
|---|---|
| `develsh` | gives **gid** 0 but uid stays 29999 — cannot write `/opt` or `/var/lib/dpkg` |
| `aegis-dpkg` | a 6-line wrapper that **exits unless `id -u` is 0**; sets `AEGIS_FIXED_ORIGIN` then calls dpkg |
| `devel-su` | real root, prompts for a password on a TTY (so it hangs under plain `ssh`) |

There is no password-free path to installing a package. In open mode the
install prints `digsigsums` warnings for every file; they are expected and
harmless.

## Screenshots

`/home/user/n9shot` writes a **PPM to stdout** — it does not take a filename:

```sh
ssh user@n9 'DISPLAY=:0 /home/user/n9shot' > shot.ppm
ffmpeg -i shot.ppm -vf transpose=1 shot.png    # transpose: framebuffer is landscape
```

Piping it into a terminal without redirecting looks exactly like a hang.

## Network

No route exists until WiFi is associated; `resolv.conf` points at `127.0.0.1`
(a local resolver that only works when a connection is up), and the USB link
has no default route. `wlan0`'s `operstate` tells you which.
