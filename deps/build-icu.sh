#!/bin/sh
# Cross-build ICU for the Nokia N9 (armv7-a / neon / hard-float, glibc 2.10, Linux 2.6.32).
#
# ICU is the canary dependency for the WebKit port: big, C++-heavy, and a hard
# requirement of the engine. If it builds and its binaries run on device, most of
# the rest of the dependency chain will follow.
#
# ICU cannot be cross-built directly -- its build runs its own generators (genrb,
# pkgdata, ...) to produce the locale data. So this does a full HOST build first
# and points the cross build at it with --with-cross-build.
set -eu

ICU_VER=70_1
ICU_SHA512=0b26ae7207155cb65a8fdb25f7b2fa4431e74b12bccbed0884a17feaae3c96833d12451064dd152197fd6ea5fd3adfd95594284a463e66c82e0d860f645880c9
ICU_URL="https://github.com/unicode-org/icu/releases/download/release-70-1/icu4c-${ICU_VER}-src.tgz"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLCHAIN=${TOOLCHAIN:-$HOME/cross/arm-harmattan}
SYSROOT=${SYSROOT:-$HOME/QtSDK/Madde/sysroots/harmattan_sysroot_10.2011.34-1_slim}
TRIPLE=arm-none-linux-gnueabi

WORK="$ROOT/build/icu"
SRC="$WORK/icu/source"
HOST_BUILD="$WORK/host"
ARM_BUILD="$WORK/arm"
PREFIX="$ROOT/out/arm/icu"          # staging; mirrors what lands in /opt/n9browser
JOBS=$(nproc 2>/dev/null || echo 4)

[ -x "$TOOLCHAIN/bin/$TRIPLE-gcc" ] || { echo "no cross gcc at $TOOLCHAIN/bin/$TRIPLE-gcc" >&2; exit 1; }
[ -d "$SYSROOT" ]                   || { echo "no sysroot at $SYSROOT" >&2; exit 1; }

mkdir -p "$WORK"

# ---- fetch (checksum pinned from the release's own SHASUM512.txt) ------------
TARBALL="$WORK/icu4c-${ICU_VER}-src.tgz"
if [ ! -f "$TARBALL" ]; then
    echo "==> fetching ICU $ICU_VER"
    curl -fL --retry 3 -o "$TARBALL.part" "$ICU_URL"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$ICU_SHA512  $TARBALL" | sha512sum -c - >/dev/null || {
    echo "ICU tarball checksum mismatch -- refusing to build. Delete $TARBALL and retry." >&2
    exit 1
}

if [ ! -d "$WORK/icu" ]; then
    echo "==> extracting"
    tar -xzf "$TARBALL" -C "$WORK"
fi

# ---- host build: only needed for the data generators ------------------------
# Must be the SAME ICU version as the cross build, or the data tools mismatch.
if [ ! -f "$HOST_BUILD/.done" ]; then
    echo "==> host build (generators only)"
    rm -rf "$HOST_BUILD"; mkdir -p "$HOST_BUILD"
    ( cd "$HOST_BUILD" && "$SRC/configure" \
        --enable-static --disable-shared \
        --disable-samples --disable-tests --disable-extras \
        >config.log 2>&1 || { tail -30 config.log; exit 1; } )
    make -C "$HOST_BUILD" -j"$JOBS"
    touch "$HOST_BUILD/.done"
fi

# ---- cross build ------------------------------------------------------------
# Toolchain defaults already carry --with-arch=armv7-a --with-fpu=neon
# --with-float=hard --with-sysroot=$SYSROOT, so only the non-default bits here:
#
#   -static-libstdc++ -static-libgcc  the device's GCC 4.x libstdc++ lacks the
#                                     GLIBCXX/CXXABI symbols GCC 14 emits
#   --dynamic-linker /lib/ld-linux.so.3
#                                     GCC defaults PT_INTERP to
#                                     /lib/ld-linux-armhf.so.3, which the N9
#                                     rootfs does not have -- without this, any
#                                     executable fails to exec at all
#   --disable-new-dtags               emit DT_RPATH, not DT_RUNPATH; only RPATH
#                                     is inherited by transitively-loaded libs
echo "==> cross build (armv7hf)"
mkdir -p "$ARM_BUILD"
( cd "$ARM_BUILD" && \
  CC="$TOOLCHAIN/bin/$TRIPLE-gcc" \
  CXX="$TOOLCHAIN/bin/$TRIPLE-g++" \
  AR="$TOOLCHAIN/bin/$TRIPLE-ar" \
  RANLIB="$TOOLCHAIN/bin/$TRIPLE-ranlib" \
  CFLAGS="-O2 -fPIC" \
  CXXFLAGS="-O2 -fPIC" \
  LDFLAGS="-static-libstdc++ -static-libgcc -Wl,--dynamic-linker,/lib/ld-linux.so.3 -Wl,-rpath,/opt/n9browser/lib -Wl,--disable-new-dtags" \
  "$SRC/configure" \
    --host=$TRIPLE \
    --with-cross-build="$HOST_BUILD" \
    --prefix="$PREFIX" \
    --enable-static --disable-shared \
    --with-data-packaging=static \
    --disable-samples --disable-tests --disable-extras \
    >config.log 2>&1 || { tail -40 config.log; exit 1; } )
make -C "$ARM_BUILD" -j"$JOBS"
make -C "$ARM_BUILD" install

# ICU is C++, but its pkg-config declares only "-lpthread -ldl -lm" as private
# libs. Anything linking static ICU from a C project (libpsl -> libsoup) then
# links with the C driver and fails on __gxx_personality_v0 / __cxa_end_cleanup /
# __cxxabiv1 vtables. Ordering rules this out as a fixable-via-LDFLAGS problem:
# static archives resolve in a single pass, so a -lstdc++ passed in LDFLAGS lands
# BEFORE libicuuc.a and cannot satisfy it. Declaring it here puts it in the right
# position on every consumer's link line.
for pc in "$PREFIX"/lib/pkgconfig/*.pc; do
    [ -f "$pc" ] || continue
    grep -q -- "-lstdc++" "$pc" || sed -i "s/^\(baselibs *=.*\)$/\1 -lstdc++/" "$pc"
done

echo
echo "==> installed to $PREFIX"
ls -la "$PREFIX/lib" | grep '\.a$' || true
echo
echo "arch check:"
file "$PREFIX/lib/libicuuc.a" 2>/dev/null || true
"$TOOLCHAIN/bin/$TRIPLE-objdump" -f "$ARM_BUILD/lib/libicuuc.a" 2>/dev/null | grep -m1 "file format" || true
