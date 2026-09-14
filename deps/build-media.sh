#!/bin/sh
# libxml2, SQLite, libjpeg-turbo, libwebp -- the parsing/storage/codec tier.
# Run deps/build-graphics.sh first.
. "$(dirname "$0")/toolchain.sh"

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

# libxml2: no Python bindings, no lzma. zlib comes from our prefix.
auto libxml2.a libxml2-2.12.6.tar.xz libxml2-2.12.6 \
    --without-python --without-lzma --without-debug --with-zlib="$PREFIX"

# SQLite: WebKit uses it for IndexedDB and local storage. The amalgamation
# needs no special options; readline is host-only tooling.
auto libsqlite3.a sqlite-autoconf-3450300.tar.gz sqlite-autoconf-3450300 \
    --disable-readline

# libwebp: WebKit decodes WebP for the wider web; many sites serve nothing else.
# WPE does find_package(WebP REQUIRED COMPONENTS demux), so demux is NOT optional.
# Guard on libwebpdemux.a rather than libwebp.a so an existing demux-less build
# gets redone.
auto libwebpdemux.a libwebp-1.3.2.tar.gz libwebp-1.3.2 \
    --enable-libwebpdemux --disable-libwebpmux \
    --disable-gl --disable-sdl --disable-png --disable-jpeg --disable-tiff --disable-gif

# libjpeg-turbo 3.x dropped autotools, so this one is CMake.
if [ ! -f "$PREFIX/lib/libjpeg.a" ]; then
    echo "==> libjpeg-turbo-3.0.2"
    d=$(unpack libjpeg-turbo-3.0.2.tar.gz libjpeg-turbo-3.0.2)
    TC=$(cmake_toolchain_file)
    b="$WORK/libjpeg-turbo-build"; rm -rf "$b"; mkdir -p "$b"
    cmake -S "$d" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$TC" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF \
        >"$WORK/libjpeg-setup.log" 2>&1 || { echo "--- cmake failed ---"; tail -25 "$WORK/libjpeg-setup.log"; exit 1; }
    cmake --build "$b" -j "$JOBS" >"$b/build.log" 2>&1 \
        || { echo "--- build failed ---"; tail -30 "$b/build.log"; exit 1; }
    cmake --install "$b" >/dev/null 2>&1
fi

echo
echo "==> media tier installed"
ls "$PREFIX/lib" | grep -E "\.a$" | tr '\n' ' '; echo
