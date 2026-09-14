#!/bin/sh
# The font + 2D graphics stack: libpng, pixman, expat, FreeType, HarfBuzz,
# fontconfig, Cairo. Everything static, image backend only -- no X11, no GL.
# That is deliberate: the engine process must link nothing from the device, and
# the N9's SGX530 is GLES2-only anyway, so all painting is CPU-side.
#
# Run deps/build-glib.sh and deps/build-icu.sh first.
. "$(dirname "$0")/toolchain.sh"

CROSS=$(meson_cross_file)

auto() {   # $1=guard lib  $2=tarball  $3=srcdir  then configure args
    guard="$1"; tb="$2"; sd="$3"; shift 3
    [ -f "$PREFIX/lib/$guard" ] && { echo "==> $sd (cached)"; return 0; }
    echo "==> $sd"
    d=$(unpack "$tb" "$sd")
    b="$WORK/$sd-build"; rm -rf "$b"; mkdir -p "$b"
    ( cd "$b" && "$d/configure" --host=$TRIPLE --prefix="$PREFIX" \
        --enable-static --disable-shared "$@" \
        >config.log 2>&1 || { echo "--- configure failed ---"; tail -30 config.log; exit 1; } )
    make -C "$b" -j"$JOBS" >"$b/make.log" 2>&1 || { echo "--- build failed ---"; tail -30 "$b/make.log"; exit 1; }
    make -C "$b" install >/dev/null 2>&1
}

mson() {   # $1=guard lib  $2=tarball  $3=srcdir  then meson args
    guard="$1"; tb="$2"; sd="$3"; shift 3
    [ -f "$PREFIX/lib/$guard" ] && { echo "==> $sd (cached)"; return 0; }
    echo "==> $sd"
    d=$(unpack "$tb" "$sd")
    b="$WORK/$sd-build"; rm -rf "$b"
    meson setup "$b" "$d" --cross-file "${CROSS_FILE:-$CROSS}" --prefix="$PREFIX" \
        --default-library=static --buildtype=release -Dprefer_static=true "$@" \
        >"$WORK/$sd-setup.log" 2>&1 || { echo "--- meson setup failed ---"; tail -25 "$WORK/$sd-setup.log"; exit 1; }
    meson compile -C "$b" -j "$JOBS" >"$b/compile.log" 2>&1 \
        || { echo "--- compile failed ---"; tail -30 "$b/compile.log"; exit 1; }
    meson install -C "$b" --no-rebuild >/dev/null 2>&1
}

auto libpng16.a libpng-1.6.43.tar.xz libpng-1.6.43

# pixman needs no libpng: that is only for its tests and demos.
mson libpixman-1.a pixman-0.43.4.tar.gz pixman-0.43.4 \
    -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled

auto libexpat.a expat-2.6.2.tar.xz expat-2.6.2 \
    --without-docbook --without-examples --without-tests

# FreeType and HarfBuzz are mutually optional. Build FreeType WITHOUT HarfBuzz,
# then HarfBuzz against it. Re-building FreeType with HarfBuzz afterwards only
# improves autohinting of complex scripts; skipped until that matters.
auto libfreetype.a freetype-2.13.2.tar.xz freetype-2.13.2 \
    --with-harfbuzz=no --with-brotli=no --with-bzip2=no \
    --with-png=yes --with-zlib=yes

mson libharfbuzz.a harfbuzz-8.3.0.tar.xz harfbuzz-8.3.0 \
    -Dglib=enabled -Dicu=enabled -Dfreetype=enabled \
    -Dgobject=disabled -Dcairo=disabled -Dchafa=disabled -Dgraphite=disabled \
    -Dtests=disabled -Ddocs=disabled -Dbenchmark=disabled \
    -Dintrospection=disabled -Dutilities=disabled

# Two fontconfig quirks:
#  - cache-build runs fc-cache, an ARM binary we cannot exec here, so disable it.
#  - its gperf input is preprocessed by the C compiler, so the force-included
#    compat header would be parsed as gperf declarations. fontconfig does no
#    long double math, so just build it without the header.
mson libfontconfig.a fontconfig-2.15.0.tar.xz fontconfig-2.15.0 \
    -Dtests=disabled -Dtools=disabled -Ddoc=disabled -Dnls=disabled \
    -Dcache-build=disabled

# Image backend only. Every windowing/GPU backend off.
mson libcairo.a cairo-1.18.0.tar.xz cairo-1.18.0 \
    -Dxlib=disabled -Dxcb=disabled -Dxlib-xcb=disabled -Dquartz=disabled \
    -Ddwrite=disabled -Dtee=disabled -Dtests=disabled -Dspectre=disabled \
    -Dsymbol-lookup=disabled -Dgtk_doc=false \
    -Dfreetype=enabled -Dfontconfig=enabled -Dpng=enabled -Dzlib=enabled \
    -Dglib=enabled

echo
echo "==> graphics stack installed"
ls "$PREFIX/lib" | grep -E "\.a$" | tr '\n' ' '; echo
