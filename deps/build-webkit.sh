#!/bin/sh
# WPE WebKit 2.44.4 -- the engine itself.
#
# 2.44 is pinned deliberately: it is the last series whose painter is Cairo
# (verified in-tree: OptionsWPE.cmake sets USE_CAIRO TRUE and there is not one
# Skia reference in Source/cmake). 2.46 made Skia the default, which assumes GL
# the SGX530 cannot provide.
#
# Everything not needed to lay out and paint a page is switched off: no media,
# no WebGL, no accessibility, no sandbox, no introspection. The goal is correct
# flexbox/grid rendering, not a daily driver.
. "$(dirname "$0")/toolchain.sh"

SRC="$WORK/wpewebkit-2.44.4"
B="$ROOT/build-wpe"
[ -d "$SRC" ] || tar -xf "$TARBALLS/wpewebkit-2.44.4.tar.xz" -C "$WORK"

# Two glibc-2.10 fixes, both needed before a single WTF file compiles.
#
# 1. std::trunc / std::round / std::log2 ... are NOT in namespace std. The cross
#    libstdc++ has _GLIBCXX_USE_C99_MATH_FUNCS and _GLIBCXX_USE_C99_MATH_TR1
#    left "#undef" in c++config.h, because its configure probe for C99 math ran
#    against this sysroot and failed -- and it failed for the SAME reason as trap
#    1 in the notes: glibc suppresses every long double prototype here, so the
#    probe saw an incomplete C99 math library. Defining the macros ALONE makes it
#    worse (4 errors -> 35), since <cmath> then does "using ::truncl;" for
#    prototypes that do not exist. Macros PLUS the compat header gives 0.
#
# 2. CLOCK_MONOTONIC_COARSE reached glibc in 2.11. The value is 6 on Linux and
#    the device's 2.6.32 kernel does support it -- verified on device, not
#    assumed: clock_gettime(6) returns a sane monotonic reading.
# 3. SCHED_IDLE: glibc 2.10's bits/sched.h stops at SCHED_BATCH (3). SCHED_IDLE
#    is 5 on Linux and the kernel has had it since 2.6.23. Passed as plain -D
#    rather than another force-included header, so there is no include-order
#    hazard (see the notes on how -include leaked four different ways).
compat_on
# -mthumb is REQUIRED, not an optimisation. The toolchain was configured
# --with-arch=armv7-a but not --with-mode=thumb, so it emits ARM code and never
# defines __thumb2__. WebKit tests for that (OptionsCommon.cmake ->
# ARM_THUMB2_DETECTED) and it gates two things:
#   - OFFLINE_ASM_BACKEND, which stays EMPTY without it, so offlineasm is invoked
#     with no backend argument and dies in generate_settings_extractor.rb
#   - ENABLE_JIT_DEFAULT (WebKitFeatures.cmake:97 requires ARM_THUMB2_DETECTED)
# JSC's 32-bit ARM JIT is CPU(ARM_THUMB2) only -- there is no JIT for ARM mode.
# Deps already built in ARM mode are fine; ARM/Thumb interwork per function.
# MADV_DONTDUMP/MADV_DODUMP are Linux 3.4+ (glibc 2.16), so unlike the constants
# above this kernel genuinely does NOT support them. Defining their real values
# is still safe: bmalloc wraps these in SYSCALL(), which retries only on EAGAIN
# and ignores every other failure, so madvise just returns EINVAL and is dropped.
# Using the real values keeps the behaviour correct on any newer kernel.
N9DEFS="-DCLOCK_MONOTONIC_COARSE=6 -DSCHED_IDLE=5 -DMADV_DONTDUMP=16 -DMADV_DODUMP=17 -mthumb"
N9COMPAT="-include $ROOT/deps/compat/n9-webkit-compat.h"
export CFLAGS="$CFLAGS $N9DEFS $N9COMPAT"
export CXXFLAGS="$CXXFLAGS $N9DEFS $N9COMPAT -D_GLIBCXX_USE_C99_MATH_FUNCS=1 -D_GLIBCXX_USE_C99_MATH_TR1=1"

# -Bsymbolic-functions binds the library's own function references internally at
# link time instead of going through the PLT. libWPEWebKit has 286k dynamic
# relocations; the UIProcess, WebProcess and NetworkProcess each process the
# whole set at load, on one 1 GHz core. It is mostly a STARTUP win -- the RAM
# saving is under 1 MB per process -- and it is passed as a cmake -D rather than
# through LDFLAGS so it changes only the link steps, not every compile.
#
# WebKit's FindGLIB (and friends) use find_library(), handing CMake a bare path
# to libglib-2.0.a -- so pkg-config's Libs.private never reaches the link line
# and static deps like pcre2 show up as undefined references. Appending them to
# CMAKE_*_STANDARD_LIBRARIES puts them at the END of every link command, which is
# the position static archives need (single-pass resolution).
# gpg-error and brotli are added by hand: their dependents are found by full path
# rather than pkg-config, so they have no .pc entry to expand.
N9LIBS=$(pkg-config --libs \
    glib-2.0 gio-2.0 gobject-2.0 cairo harfbuzz harfbuzz-icu fontconfig \
    freetype2 libxml-2.0 sqlite3 libsoup-3.0 icu-uc icu-i18n libpsl libjpeg \
    libpng libwebp libwebpdemux xkbcommon epoxy libcrypto libssl 2>/dev/null \
    | tr ' ' '\n' | grep '^-l' | awk '!seen[$0]++' | tr '\n' ' ')
# woff2: WebCore links only WOFF2::dec (see WebCore/CMakeLists.txt "if (USE_WOFF2)"),
# but the decoder's helpers -- ComputeULongSum, Read255UShort, ReadBase128 -- live
# in woff2common, so it has to be added by hand.
# n9wpebackend: libwpe was built static, so meson selected loader-static.c, which
# requires the linker to find a _wpe_loader_interface symbol. Our backend provides
# it (currently a skeleton; see backend/n9-wpe-backend.c).
N9LIBS="$N9LIBS -lgpg-error -lbrotlidec -lbrotlicommon -lwoff2common -ln9compat"
# The WPE backend is NOT linked in: libwpe is shared and dlopens
# libWPEBackend-n9.so at runtime, so WebKit needs no _wpe_loader_interface at
# link time and X11/EGL are the backend module's own dependencies.

# EGL native type fix: see out/arm/sysroot/include/EGL/eglplatform.h, a shadowing
# copy of the sysroot header with one typedef corrected (EGLNativeDisplayType is
# "int" in the PowerVR NULLWS block, but modern eglGetPlatformDisplay takes
# void*). Our include dir is searched before the device sysroot, so it wins.

# ---- SGX530 fragment-uniform budget ----------------------------------------
# Two problems with WebKit's rounded-rect clip shader on this GPU, found by
# replaying the captured shader against the driver in isolation
# (probe/glcompile.c, shaders captured with probe/n9crash.c):
#
#  1. It indexes u_roundedRect[] and u_roundedRectInverseTransformMatrix[] with
#     a LOOP VARIABLE. Dynamic indexing of a uniform array makes this driver's
#     GLSL compiler SEGFAULT inside GLSLCompileToUniflex instead of reporting an
#     error -- killing the web process on any page with two or more nested
#     rounded-corner clips. Measured: byte-identical shader, constant indices,
#     compiles fine. That is the actual fix; unrolling costs a little shader
#     size and keeps all the clips.
#
#  2. Its arrays are sized for 10 simultaneous clips: 30 vec4 + 10 mat4 = 70
#     vec4, against GL_MAX_FRAGMENT_UNIFORM_VECTORS = 64 (measured,
#     probe/gllimits.c). Four clips need 28 and fit. Beyond four,
#     ClipStack::addRoundedRect declines and corners render square -- cosmetic,
#     and only when nesting deeply.
#
# Done here rather than by hand so it survives re-unpacking the tarball, and
# idempotently so re-running costs nothing. All four constants must agree --
# WebKit says so in ClipStack.h and does not check.
patch_rounded_rect_limit() {
    python3 - "$SRC" <<'PYEOF'
import sys, os
src = sys.argv[1]
cs = os.path.join(src, "Source/WebCore/platform/graphics/texmap/ClipStack.h")
sp = os.path.join(src, "Source/WebCore/platform/graphics/texmap/TextureMapperShaderProgram.cpp")

t = open(sp).read()
if "n9: unrolled for SGX530" in t:
    raise SystemExit(0)          # already patched

c = open(cs).read()
c = c.replace("s_roundedRectMaxClips = 10;", "s_roundedRectMaxClips = 4;")
open(cs, "w").write(c)

t = t.replace("define ROUNDED_RECT_MAX_RECTS 10", "define ROUNDED_RECT_MAX_RECTS 4")
t = t.replace("define ROUNDED_RECT_ARRAY_SIZE 30", "define ROUNDED_RECT_ARRAY_SIZE 12")
t = t.replace("define ROUNDED_RECT_INVERSE_TRANSFORM_ARRAY_SIZE 10",
              "define ROUNDED_RECT_INVERSE_TRANSFORM_ARRAY_SIZE 4")

loop_start = t.index("            for (int rectIndex = 0; rectIndex < ROUNDED_RECT_MAX_RECTS; rectIndex++) {")
loop_end = t.index("            }\n", t.index("color *= roundedRectCoverage(fragCoord.xy, bounds,", loop_start)) + len("            }\n")

block = """            // n9: unrolled for SGX530. The loop above indexed
            // u_roundedRectInverseTransformMatrix and u_roundedRect with a
            // LOOP VARIABLE. Dynamic indexing of a uniform array segfaults this
            // device's GLSL compiler (libglslcompiler, inside GLSLCompileToUniflex)
            // rather than failing to compile -- taking the whole web process with
            // it on any page with two or more nested rounded-corner clips.
            // Measured: identical shader compiles fine with constant indices.
            // Costs a little shader size; keeps all four clips.
"""
for i in range(4):
    block += """            if (u_roundedRectNumber > %(i)d) {
                vec4 fragCoord%(i)d = u_roundedRectInverseTransformMatrix[%(i)d] * v_nonProjectedPosition;
                vec4 bounds%(i)d = vec4(u_roundedRect[%(a)d].xy, u_roundedRect[%(a)d].xy + u_roundedRect[%(a)d].zw);
                color *= roundedRectCoverage(fragCoord%(i)d.xy, bounds%(i)d, u_roundedRect[%(b)d].xy, u_roundedRect[%(b)d].zw, u_roundedRect[%(c)d].xy, u_roundedRect[%(c)d].zw);
            }
""" % {"i": i, "a": i * 3, "b": i * 3 + 1, "c": i * 3 + 2}

t = t[:loop_start] + block + t[loop_end:]
open(sp, "w").write(t)
print("==> patched rounded-rect clip: limit 10 -> 4, loop unrolled to constant indices")
PYEOF
}

patch_rounded_rect_limit

# Process-swap-on-navigation, off.
#
# WebKit starts a fresh web process for every cross-site navigation and keeps
# the old ones a while. Measured on device, five loads alternating between two
# hosts: FOUR live WPEWebProcess at once, free memory 101 -> 58 MB, a 40-second
# stall, and one navigation lost outright. Ordinary browsing hits it every few
# links. See docs/process-swap.md for the full measurement and for the three
# runtime routes that are all closed -- the GLib feature API does not expose
# this preference, the WebKit C API is not built into the library, and no
# environment variable reaches it. Patching the source is what is left.
#
# The preference itself defaults to true and is code-generated, so the cheapest
# surgical point is where the pool reads it. Everything else follows: the web
# process cache is already disabled below 3 GB of RAM, and automatic process
# prewarming is gated on this same flag.
#
# The cost is site isolation, which is what the feature is for. On a single core
# with ~100 MB free and JavaScript off by default, the memory is worth more.
patch_disable_process_swap() {
    python3 - "$SRC" <<'PYEOF'
import sys, os
src = sys.argv[1]
p = os.path.join(src, "Source/WebKit/UIProcess/API/glib/WebKitWebContext.cpp")
t = open(p).read()
if "n9: process swap disabled" in t:
    raise SystemExit(0)          # already patched

# The WPE branch of the #if. GTK reads a property here; WPE hardcodes true.
old = "    configuration.setProcessSwapsOnNavigation(true);"
if old not in t:
    raise SystemExit("process-swap patch site not found; check WebKitWebContext.cpp")

new = ("    // n9: process swap disabled. A web process per cross-site navigation\n"
       "    // does not fit in ~100 MB of free RAM -- four end up alive at once,\n"
       "    // the device stalls for tens of seconds and loses navigations.\n"
       "    // See docs/process-swap.md. Prewarming and the process cache are\n"
       "    // gated on the same flag, so they go too.\n"
       "    //\n"
       "    // THIS is the deciding call, not the preference in WebProcessPool:\n"
       "    // processSwapsOnNavigation() is m_processSwapsOnNavigationFromClient\n"
       "    // .value_or(fromExperimentalFeatures), and this setter writes the\n"
       "    // CLIENT half, which wins. Patching the experimental half instead\n"
       "    // builds, installs and changes nothing at all.\n"
       "    configuration.setProcessSwapsOnNavigation(false);")
t = t.replace(old, new, 1)
open(p, "w").write(t)
print("==> patched process-swap-on-navigation: forced off in the WPE context")
PYEOF
}

patch_disable_process_swap

# Let the environment override the RAM size WebKit sizes itself from.
#
# The device reports 985 MB and has ~150 MB free, and everything RAM-proportional
# believes the 985: JSC heap growth, the memory cache, the back/forward cache
# count, the memory-kill thresholds. At 985 MB CacheModel.cpp gives a 16 MB
# memory cache and TWO bfcache documents; at 256 it gives 8 MB and one.
#
# Patched in ramSize(), NOT in computeRAMSize()'s Linux branch as first planned:
# this build has USE_SYSTEM_MALLOC 0, so that branch is #if'd out entirely and
# computeRAMSize() returns bmalloc::api::availableMemory(). Patching the branch
# would have compiled, installed and done nothing. ramSize() is the single
# funnel every caller goes through.
patch_ram_size() {
    python3 - "$SRC" <<'PYEOF'
import sys, os
src = sys.argv[1]
p = os.path.join(src, "Source/WTF/wtf/RAMSize.cpp")
t = open(p).read()
if "n9: WEBKIT_RAM_SIZE_MB" in t:
    raise SystemExit(0)          # already patched

old = """size_t ramSize()
{
    static size_t ramSize;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        ramSize = computeRAMSize();
    });
    return ramSize;
}"""
if old not in t:
    raise SystemExit("ramSize patch site not found; check RAMSize.cpp")

new = """size_t ramSize()
{
    static size_t ramSize;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        // n9: WEBKIT_RAM_SIZE_MB overrides the perceived RAM size. The N9
        // reports 985 MB and has ~150 MB of it free, so every RAM-proportional
        // default here is sized for memory that does not exist. This is the
        // source-level equivalent of JSC_forceRAMSize, except that it also
        // reaches WebCore -- the memory cache and the back/forward cache count
        // are computed from this number too (Shared/CacheModel.cpp).
        if (const char* n9Override = getenv("WEBKIT_RAM_SIZE_MB")) {
            long n9Mb = atol(n9Override);
            if (n9Mb > 0) {
                ramSize = static_cast<size_t>(n9Mb) * 1024 * 1024;
                return;
            }
        }
        ramSize = computeRAMSize();
    });
    return ramSize;
}"""
t = t.replace(old, new, 1)

# getenv/atol: the file includes neither today.
inc = "#include <wtf/RAMSize.h>"
if inc not in t:
    raise SystemExit("RAMSize.cpp include site not found")
t = t.replace(inc, inc + "\n#include <cstdlib>   // n9: WEBKIT_RAM_SIZE_MB", 1)

open(p, "w").write(t)
print("==> patched WTF::ramSize(): honours WEBKIT_RAM_SIZE_MB")
PYEOF
}

patch_ram_size

# Long press on a link, which WPE generates and then discards.
#
# TouchGestureController turns a press held past 500 ms into a ContextMenuEvent
# correctly, but PageClientImpl's handler for it is an empty lambda carrying
# "FIXME: Generate contextmenuevent without accidentally generating
# mouseup/mousedown events". So the gesture exists, reaches the port, and dies
# there: WebKitWebView::context-menu can never fire from touch, and no amount of
# work in the engine or the shell can make it.
#
# The FIXME is about not delivering a stray left click to the page. Using the
# RIGHT button avoids that: WebCore raises the context menu from button 2 and
# pages do not act on it the way they act on a left click.
patch_touch_context_menu() {
    python3 - "$SRC" <<'PYEOF'
import sys, os
src = sys.argv[1]
p = os.path.join(src, "Source/WebKit/UIProcess/API/wpe/PageClientImpl.cpp")
t = open(p).read()
if "n9: long press" in t:
    raise SystemExit(0)          # already patched

old = """        [&](TouchGestureController::ContextMenuEvent&) {
            // FIXME: Generate contextmenuevent without accidentally generating mouseup/mousedown events
        },"""
if old not in t:
    raise SystemExit("touch context-menu patch site not found; check PageClientImpl.cpp")

new = """        [&](TouchGestureController::ContextMenuEvent& contextMenuEvent) {
            // n9: long press. Upstream leaves this empty, so the gesture is
            // generated and dropped and context-menu never fires from touch.
            // The right button is what keeps the upstream FIXME honest: WebCore
            // raises the context menu from it, and a page does not treat it as
            // an activation the way it would a left click.
            auto* event = &contextMenuEvent.event;
            // Move the pointer there FIRST, as the click path above does.
            // Without it WebCore hit-tests wherever it last thought the
            // pointer was, so no menu is raised for the node under the finger.
            event->type = wpe_input_pointer_event_type_motion;
            page.handleMouseEvent(NativeWebMouseEvent(event, page.deviceScaleFactor(), WebMouseEventSyntheticClickType::NoTap));
            event->type = wpe_input_pointer_event_type_button;
            event->button = 2;
            event->state = 1;
            event->modifiers |= wpe_input_pointer_modifier_button2;
            page.handleMouseEvent(NativeWebMouseEvent(event, page.deviceScaleFactor(), WebMouseEventSyntheticClickType::NoTap));
            event->state = 0;
            event->modifiers &= ~wpe_input_pointer_modifier_button2;
            page.handleMouseEvent(NativeWebMouseEvent(event, page.deviceScaleFactor(), WebMouseEventSyntheticClickType::NoTap));
        },"""
t = t.replace(old, new, 1)
open(p, "w").write(t)
p2 = os.path.join(src, "Source/WebKit/UIProcess/wpe/WebContextMenuProxyWPE.h")
t2 = open(p2).read()
if "n9: let the base class" not in t2:
    old2 = "    void showContextMenuWithItems(Vector<Ref<WebContextMenuItem>>&&) final { }\n    void show() final { };"
    if old2 not in t2:
        raise SystemExit("context menu proxy patch site not found; check WebContextMenuProxyWPE.h")
    new2 = ("    // n9: let the base class run. WebContextMenuProxy::show() is what calls\n"
            "    // getContextMenuFromProposedMenu, which is what reaches the GLib client\n"
            "    // and emits WebKitWebView::context-menu. Overriding show() with an empty\n"
            "    // body -- as this port did -- silently discards every context menu, so\n"
            "    // the whole API is present and inert: the signal can never fire.\n"
            "    //\n"
            "    // showContextMenuWithItems stays empty on purpose. That is the step that\n"
            "    // DRAWS a native menu, and the menu here is the platform QML one.\n"
            "    void showContextMenuWithItems(Vector<Ref<WebContextMenuItem>>&&) final { }")
    t2 = t2.replace(old2, new2, 1)
    open(p2, "w").write(t2)
    print("==> patched context menu proxy: the signal can now fire")
print("==> patched touch long press: context menu is actually delivered")
PYEOF
}

patch_touch_context_menu

TC=$(cmake_toolchain_file)

# CMAKE_<LANG>_FLAGS_INIT seeds the cache only on the FIRST configure of a build
# directory. Re-running cmake over an existing build-wpe therefore keeps the old
# flags, silently, and the build fails on whatever the new flag was meant to fix.
# Stamp the flags and wipe the directory when they change.
STAMP="$B/.n9flags"
WANT="$CFLAGS|$CXXFLAGS|$LDFLAGS|$(cat "$TC")"
if [ -d "$B" ] && [ "$(cat "$STAMP" 2>/dev/null)" != "$WANT" ]; then
    echo "build flags changed -- reconfiguring from scratch"
    rm -rf "$B"
fi

# JIT is ON. WTF/PlatformEnable.h turns it on by default for exactly this target:
#   #if CPU(ARM_THUMB2) && CPU(ARM_HARDFP) && OS(LINUX)
#   /* On ARMv7 Linux the JIT is enabled unless explicitly disabled. */
# and the device permits it -- verified by probe: RWX mmap OK, RW->RX mprotect
# OK, and hand-written Thumb code mmapped and executed correctly, so aegis does
# not block executable pages. This gives three tiers (LLInt assembly interpreter
# -> Baseline -> DFG) instead of the C++ CLoop. FTL stays off: it needs 64-bit.
cmake -S "$SRC" -B "$B" -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$TC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEVPREFIX" \
    -DPORT=WPE \
    -DENABLE_JIT=ON -DENABLE_C_LOOP=OFF -DENABLE_DFG_JIT=ON -DENABLE_SAMPLING_PROFILER=OFF \
    -DENABLE_WPE_PLATFORM=OFF -DENABLE_WPE_QT_API=OFF \
    -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DENABLE_MEDIA_STREAM=OFF \
    -DENABLE_MEDIA_SOURCE=OFF -DENABLE_ENCRYPTED_MEDIA=OFF \
    -DENABLE_WEBGL=OFF -DENABLE_WEBXR=OFF -DENABLE_WEB_CODECS=OFF \
    -DENABLE_GAMEPAD=OFF -DENABLE_SPEECH_SYNTHESIS=OFF \
    -DENABLE_ACCESSIBILITY=OFF -DUSE_ATK=OFF -DENABLE_INTROSPECTION=OFF \
    -DENABLE_DOCUMENTATION=OFF -DENABLE_MINIBROWSER=OFF \
    -DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_JOURNALD_LOG=OFF \
    -DENABLE_XSLT=OFF \
    # ENABLE_NOTIFICATIONS is deliberately NOT here. Turning it off fails the
    # build: PlatformEnable.h asserts NOTIFICATION_EVENT requires NOTIFICATIONS,
    # and PlatformEnableGlib.h hardcodes ENABLE_NOTIFICATION_EVENT 1 for every
    # GLib port with no cmake option to turn it off. It would need a fourth
    # source patch for an unmeasured code-size gain; options before patches.
    -DENABLE_REMOTE_INSPECTOR=OFF -DENABLE_GEOLOCATION=OFF \
    -DUSE_SYSTEMD=OFF -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF -DUSE_LCMS=OFF \
    -DUSE_LIBDRM=OFF -DUSE_GBM=OFF -DUSE_OPENJPEG=OFF -DUSE_LIBBACKTRACE=OFF \
    -DUSE_WOFF2=ON -DUSE_SOUP2=OFF \
    -DENABLE_WEBDRIVER=OFF \
    -DUSE_XDGMIME=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS -Wl,-Bsymbolic-functions" \
    -DCMAKE_C_STANDARD_LIBRARIES="$N9LIBS" \
    -DCMAKE_CXX_STANDARD_LIBRARIES="$N9LIBS" \
    "$@"

mkdir -p "$B" && printf %s "$WANT" > "$STAMP"
