#!/bin/sh
# GLib and its prerequisites (zlib, libffi, PCRE2), cross-built for the N9.
#
# GLib is the SECOND canary, and a different one from ICU. ICU proved the
# toolchain can build big modern C++ against a 2009 sysroot. GLib probes the
# remaining risk: modern *syscall* usage against Linux 2.6.32 -- statx,
# memfd_create, getrandom, pipe2, and friends. These fail at RUNTIME, not at
# link, so building it is only half the test; run probe/glibprobe on device.
. "$(dirname "$0")/toolchain.sh"

# GLib 2.78's gdbus-codegen still does "import distutils.version", removed from
# Python 3.12+. setuptools ships the compatibility shim, so the venv needs it.
python3 -c "import distutils.version" 2>/dev/null || pip -q install setuptools

# ---- zlib -------------------------------------------------------------------
# Built static on purpose: the device Qt already loads the stock libz.so.1, and
# ld.so reuses the first-loaded soname process-wide, so a bundled *shared*
# modern zlib would silently lose to the sysroot copy.
if [ ! -f "$PREFIX/lib/libz.a" ]; then
    echo "==> zlib"
    d=$(unpack zlib-1.3.1.tar.gz zlib-1.3.1)
    ( cd "$d" && ./configure --static --prefix="$PREFIX" >config.log 2>&1 \
        || { tail -20 config.log; exit 1; } )
    make -C "$d" -j"$JOBS" >/dev/null && make -C "$d" install >/dev/null
fi

# ---- libffi -----------------------------------------------------------------
if [ ! -f "$PREFIX/lib/libffi.a" ]; then
    echo "==> libffi"
    d=$(unpack libffi-3.4.6.tar.gz libffi-3.4.6)
    b="$WORK/libffi-build"; rm -rf "$b"; mkdir -p "$b"
    ( cd "$b" && "$d/configure" --host=$TRIPLE --prefix="$PREFIX" \
        --enable-static --disable-shared --disable-docs \
        >config.log 2>&1 || { tail -30 config.log; exit 1; } )
    make -C "$b" -j"$JOBS" >/dev/null && make -C "$b" install >/dev/null
fi

# ---- PCRE2 ------------------------------------------------------------------
if [ ! -f "$PREFIX/lib/libpcre2-8.a" ]; then
    echo "==> pcre2"
    d=$(unpack pcre2-10.44.tar.bz2 pcre2-10.44)
    b="$WORK/pcre2-build"; rm -rf "$b"; mkdir -p "$b"
    ( cd "$b" && "$d/configure" --host=$TRIPLE --prefix="$PREFIX" \
        --enable-static --disable-shared --enable-unicode \
        >config.log 2>&1 || { tail -30 config.log; exit 1; } )
    make -C "$b" -j"$JOBS" >/dev/null && make -C "$b" install >/dev/null
fi

# ---- GLib -------------------------------------------------------------------
# GLib is the one package that needs the long-double compat header: its bundled
# gnulib probes frexpl with a declaration test and hard-errors "missing or broken
# beyond repair" without it. Everything else builds without it (see toolchain.sh).
compat_on
echo "==> glib"
d=$(unpack glib-2.78.6.tar.xz glib-2.78.6)

# glibc 2.10 exports fallocate() but never shipped fallocate64() -- that arrived
# in 2.11. Under -D_FILE_OFFSET_BITS=64 (which GLib sets) fcntl.h redirects
# fallocate -> fallocate64, so every use fails to link. Meson's cc.has_function()
# is called with no prefix here, so it link-tests its own "char fallocate();"
# stub and never sees the redirect -- it reports the function present and the
# build then breaks at link time.
#
# GLib uses fallocate only to preallocate space in write_to_file(), a pure
# optimisation, so drop it from the probe list and take GLib's fallback path.
# If other packages later need real *64 large-file symbols, the alternative is a
# small compat lib implementing them over syscall() -- note ARM EABI requires
# 64-bit syscall args in even-aligned register pairs, which syscall()'s varargs
# does not arrange for you.
if grep -q "^  'fallocate'," "$d/meson.build"; then
    sed -i "s/^  'fallocate',/  # 'fallocate',  -- see deps\/build-glib.sh: no fallocate64 in glibc 2.10/" "$d/meson.build"
    echo "    patched out fallocate detection"
fi
CROSS=$(meson_cross_file)
b="$WORK/glib-build"; rm -rf "$b"
# SHARED, not static, and this is load-bearing. GObject keeps a process-global
# type registry. With a static GLib, libWPEWebKit.so gets one copy (hidden by
# -fvisibility=hidden) and the browser app gets another, so a WebKitWebView
# registered inside WebKit is invisible to the app -- g_signal_connect fails with
# "signal 'load-changed' is invalid for instance ... of type '(NULL)'".
# Same reasoning as libwpe: shared state demands a shared library.
meson setup "$b" "$d" \
    --cross-file "$CROSS" \
    --prefix="$PREFIX" \
    --default-library=shared \
    --buildtype=release \
    -Dtests=false -Dglib_debug=disabled -Dman=false \
    -Dselinux=disabled -Dlibmount=disabled -Dnls=disabled \
    -Dxattr=false -Ddtrace=false -Dsystemtap=false
meson compile -C "$b" -j "$JOBS"
meson install -C "$b" --no-rebuild

echo
echo "==> installed to $PREFIX"
ls "$PREFIX/lib" | grep -E "^lib(glib|gobject|gio|gmodule|z|ffi|pcre2)" || true

# The whole point of this build: which modern syscalls did GLib compile in?
echo
echo "=== modern syscalls GLib configured itself to use ==="
for s in statx memfd_create getrandom pipe2 copy_file_range renameat2 close_range; do
    if grep -qs "HAVE_$(echo $s | tr a-z A-Z) 1" "$b/config.h" 2>/dev/null; then
        echo "  USES  $s   <-- must be verified against kernel 2.6.32 at runtime"
    else
        echo "  skips $s"
    fi
done
