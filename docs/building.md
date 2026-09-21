# Building

## Prerequisites

| | |
|---|---|
| Cross toolchain | GCC 14.2.0 at `~/cross/arm-harmattan`, `arm-none-linux-gnueabi`, armv7-a/neon/hard-float, **sysroot = the Madde Harmattan sysroot** |
| Harmattan SDK | `~/QtSDK/Madde`, target `harmattan_10.2011.34-1_rt1.2` — provides GCC 4.4, Qt 4.7.4, and the sysroot both toolchains use |
| Host tools | `ninja` and `meson` in `.venv/bin`; `ruby`, `unifdef`, `gperf`, `libglib2.0-dev-bin` |
| Packaging | `dpkg-deb`, `fakeroot` |

`libglib2.0-dev-bin` matters: the build needs a **host** `glib-compile-resources`.
The cross build produces an ARM one, and running it exits 126.

## Order

```sh
deps/build-icu.sh        # first: many things link it
deps/build-glib.sh
deps/build-graphics.sh   # cairo, freetype, harfbuzz, fontconfig, libpng...
deps/build-net.sh        # openssl, libpsl, nghttp2, libsoup3, glib-networking
deps/build-webkit-deps.sh
deps/build-webkit.sh     # CONFIGURES only
ninja -C build-wpe && DESTDIR=out/arm/stage ninja -C build-wpe install
backend/build-backend.sh
app/build-app.sh
shell/build-shell.sh     # goes through `mad`, uses the SDK's GCC 4.4
./package.sh             # builds everything again and wraps a .deb
deps/check-deps.sh       # audit: PIC, shared/static policy, one GType registry
```

`deps/build-webkit.sh` only runs `cmake`. The compile is a separate `ninja`
step. That is easy to miss and looks like "the build finished in 20 seconds".

**`ninja` is not on `PATH`** -- it is in `.venv/bin`. Invoking it bare fails
with "command not found", and if that is inside a pipeline (`ninja ... | tail`)
the exit status belongs to the pipeline's last command, so the build looks like
it succeeded and nothing was compiled.

**`DESTDIR` must be ABSOLUTE.** `ninja -C build-wpe` changes directory, so a
relative `DESTDIR=out/arm/stage` installs into
`build-wpe/out/arm/stage/...` while printing exactly the `-- Installing:` lines
you expect. The staged tree then silently keeps the previous build, which
presents as "my patch is not in the artifact" long after the build was fine.
Use `DESTDIR=$PWD/out/arm/stage`.

## Traps in the build system itself

**`deps/toolchain.sh` derives `$ROOT` from `$0`.** Sourcing it from a script
that lives outside the project silently builds a `PATH` pointing at a `.venv`
that is not there, and every tool "is not found". Scripts that source it must
live in the project, or set `PATH` themselves.

**The flags stamp wipes the build directory.** `CMAKE_<LANG>_FLAGS_INIT` seeds
the cache only on the *first* configure, so re-running cmake over an existing
build directory silently keeps the old flags. `build-webkit.sh` therefore stamps
`$CFLAGS|$CXXFLAGS|$LDFLAGS|<toolchain file>` and deletes the build directory
when it changes. Consequence: **changing anything in `LDFLAGS` — including the
rpath, hence `DEVPREFIX` — costs a full two-hour rebuild.** Pass new linker
flags via `-DCMAKE_SHARED_LINKER_FLAGS` / `-DCMAKE_EXE_LINKER_FLAGS` instead if
you do not want that.

**Build with `-j5`, not `-j6`.** WebCore's unified sources take 2–3 GB each and
the build host has no swap; the OOM killer takes out `cc1plus` and the whole run
is wasted. Symptom: `fatal error: Killed signal terminated program cc1plus`.

**Do not estimate a rebuild with `ninja -n | wc -l`.** It counts output lines,
not edges, and a command spans several. Worse, the real total shrinks as the run
proceeds — `[1/522]` became `[3/34]` became `[6/18]` for a one-line change in
`WebProcessPool.cpp` — because `restat` generators produce identical output and
ninja prunes the subtree it had conservatively marked dirty. Overestimating here
is what talks you out of a cheap fix.

**Rebuild scope is bounded.** `add_definitions()` in
`Source/WebKit/PlatformWPE.cmake` is directory-scoped, so changing
`PKGLIBEXECDIR` dirties ~365 objects, not the whole tree. Touching only
`TextureMapperShaderProgram.cpp` is ~40 edges plus the link. Do not assume every
WebKit change is two hours.

**The compat header is opt-in.** `deps/compat/n9-longdouble.h` declares the 51
long-double math functions glibc 2.10 suppresses under
`__NO_LONG_DOUBLE_MATH` (the symbols exist in libm; only the prototypes are
hidden). It is force-included with `-include` for GLib and WebKit **only**,
because `-include` leaked into four other builds in ugly ways: libpng's `.S`
files, fontconfig's gperf input, libtasn1's gnulib make variables. The header
therefore includes no system headers of its own, guards on
`__LDBL_MANT_DIG__ == __DBL_MANT_DIG__`, and wraps everything in
`#ifndef __ASSEMBLER__`.

**Static/shared policy is not stylistic.** Leaf libraries are static; anything
with process-global state **must** be shared:

- GLib/GObject/GIO — two copies means two GType registries and
  `type '(NULL)'` errors at runtime.
- libwpe — two copies means two backend registries.
- libsoup3 — WebKit's public API exposes Soup types.

`deps/check-deps.sh` audits this (every `.a` is PIC, the named libraries are
shared, `libgobject` exports `g_type_register_static`). It caught libsoup being
static before that cost anything. Run it after touching the dependency stack.

Related: do **not** pass `-Dprefer_static=true` to meson. With our GLib shared,
it makes meson fall through to the *device sysroot's* GLib 2.28 from 2011, which
is non-PIC and fails with "unresolvable R_ARM_JUMP24 relocation".

## The WebKit patch

`deps/build-webkit.sh` applies `patch_rounded_rect_limit()` before configuring.
It is idempotent (checks for its own marker) and self-verifying, so it survives
re-unpacking the tarball. It does two things:

1. **Unrolls the rounded-rect clip loop to constant indices.** This is the
   important one: dynamic indexing of a uniform array segfaults this device's
   GLSL compiler. See [device.md](device.md).
2. Reduces the clip limit from 10 to 4, so the uniform arrays fit in the
   64-vector fragment budget. Four constants must agree — `s_roundedRectMaxClips`
   in `ClipStack.h` and three `#define`s in `TextureMapperShaderProgram.cpp`.
   WebKit says so in a comment and does not check.

Beyond four clips, `ClipStack::addRoundedRect` declines and corners render
square: cosmetic, and only when nesting deeply.

## Deploying and packaging

`deploy.sh` assembles the on-device tree; `package.sh` wraps it in a `.deb`.
`STAGE_ONLY=1 deploy.sh` stops before touching the device, which is how
`package.sh` gets the payload.

**The install prefix stays `/opt/n9browser`** even though the app is WebMee.
WebKit bakes that path in as `PKGLIBEXECDIR` at compile time and a release build
has **no runtime override** — `WEBKIT_EXEC_PATH` exists but is inside
`#if ENABLE(DEVELOPER_MODE)` and is compiled out. Moving the prefix means
recompiling `Source/WebKit`.

**Preserve the `lib/wpe-webkit-2.0/` level.** WebKit `dlopen`s the injected
bundle by an absolute path. Flattening that directory ships it to
`lib/injected-bundle/` and every web process starts without its bundle — which
surfaces much later as an unexplained crash, not a load error. `deploy.sh` now
asserts five critical files exist before packing.

**`dpkg-deb -Zgzip` is mandatory.** Harmattan's dpkg is from 2011 and cannot
read the zstd/xz that modern `dpkg-deb` defaults to. Such a package installs
fine on a desktop and fails on the device.

**`gio-querymodules` runs on the device**, from `postinst` — it is an ARM binary
and cannot run at build time. Without the cache GIO still works; it just loads
every module in the directory instead of consulting it. The same ARM-binary trap
appears at `meson install` time and is tolerated there.

`Depends:` were taken from `dpkg -S` on the device, not guessed:
`libqt4-declarative, libqt4-opengl, libxcomposite1, libsignon-qt0,
applauncherd-invoker, applauncherd-launcher`. Everything else the engine needs
is bundled, because none of it exists on a 2011 device at a usable version.

## Iterating without reinstalling

`/opt` is root-owned after a package install, so `scp` into it fails. Two escapes:

- `N9_ENGINE=/home/user/webmee-engine` runs a freshly built engine against the
  installed tree.
- Libraries are harder: **`DT_RPATH` is searched before `LD_LIBRARY_PATH`**
  (we link with `--disable-new-dtags`), so a rebuilt `libWPEBackend-n9.so` in a
  writable directory can never win over `/opt`. The backend and WebKit can only
  be tested by installing.

`scp` of a **running** binary fails with "text file busy". Kill the app first —
and do not hide the error with `2>/dev/null`, which turns this into a confusing
"my change had no effect".
