# Shared cross-build environment. Source this; don't run it.
#
# The toolchain at ~/cross/arm-harmattan was configured with
#   --with-arch=armv7-a --with-fpu=neon --with-float=hard --with-sysroot=<madde>
# so arch/sysroot flags are already defaults and are NOT repeated here.
# Only the non-default contract is set below:
#
#   -static-libstdc++ -static-libgcc   device GCC 4.x libstdc++ lacks the
#                                      GLIBCXX/CXXABI symbols GCC 14 emits
#   --dynamic-linker /lib/ld-linux.so.3
#                                      GCC defaults PT_INTERP to
#                                      /lib/ld-linux-armhf.so.3, absent on the N9
#   --disable-new-dtags                emit DT_RPATH (inherited by transitively
#                                      loaded libs), not DT_RUNPATH
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLCHAIN=${TOOLCHAIN:-$HOME/cross/arm-harmattan}
SYSROOT=${SYSROOT:-$HOME/QtSDK/Madde/sysroots/harmattan_sysroot_10.2011.34-1_slim}
TRIPLE=arm-none-linux-gnueabi
PREFIX="$ROOT/out/arm/sysroot"        # everything we cross-build lands here
TARBALLS="$ROOT/build/tarballs"
WORK="$ROOT/build"
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}   # override: JOBS=3 ./deps/build-x.sh
DEVPREFIX=/opt/n9browser              # where it all lives on the device

export CC="$TOOLCHAIN/bin/$TRIPLE-gcc"
export CXX="$TOOLCHAIN/bin/$TRIPLE-g++"
export AR="$TOOLCHAIN/bin/$TRIPLE-ar"
export RANLIB="$TOOLCHAIN/bin/$TRIPLE-ranlib"
export STRIP="$TOOLCHAIN/bin/$TRIPLE-strip"
export NM="$TOOLCHAIN/bin/$TRIPLE-nm"
export READELF="$TOOLCHAIN/bin/$TRIPLE-readelf"
COMPAT="-include $ROOT/deps/compat/n9-longdouble.h"

# _FILE_OFFSET_BITS=64 belongs in the BASE flags so that configure/meson feature
# checks run under the same flags as the real build. glibc 2.10's fcntl.h
# redirects fallocate() -> fallocate64() under this define, but glibc 2.10 never
# shipped fallocate64 (it arrived in 2.11). Detected without the flag, the
# function looks present; compiled with it, the link fails. Same trap applies to
# every *64 large-file symbol, so set it once, globally, for everything.
LFS="-D_FILE_OFFSET_BITS=64"
BASEFLAGS="-O2 -fPIC $LFS"
# The compat header is OPT-IN, not global. Force-including it everywhere leaked
# into four separate build stages:
#   1. feature-test macros  - it pulled <features.h> in ahead of a source file's
#      own #define _GNU_SOURCE, flipping strerror_r's signature (now fixed by
#      the header including nothing at all)
#   2. .S assembly sources  - the assembler cannot parse C declarations
#   3. fontconfig's gperf input, preprocessed by cpp - 41 declarations were
#      parsed as gperf's declarations section, producing a garbage hash table
#   4. libtasn1's gnulib configure, which captured the header's CONTENTS into a
#      make variable, putting the declarations inline on the command line
# Only GLib ever actually needed it (gnulib's frexpl probe), so default to off
# and let a package opt in with compat_on.
export CFLAGS="$BASEFLAGS"
export CXXFLAGS="$BASEFLAGS"
COMPAT_OFF=1

compat_off() { COMPAT_OFF=1; export CFLAGS="$BASEFLAGS"; export CXXFLAGS="$BASEFLAGS"; }
compat_on()  { COMPAT_OFF=0; export CFLAGS="$BASEFLAGS $COMPAT"; export CXXFLAGS="$BASEFLAGS $COMPAT"; }
export CPPFLAGS="-I$PREFIX/include -I$ROOT/out/arm/icu/include"
export LDFLAGS="-L$PREFIX/lib -L$ROOT/out/arm/icu/lib -static-libstdc++ -static-libgcc -Wl,--dynamic-linker,/lib/ld-linux.so.3 -Wl,-rpath,$DEVPREFIX/lib -Wl,--disable-new-dtags"

# pkg-config must see ONLY what we cross-built, never the host's /usr/lib.
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig:$ROOT/out/arm/icu/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=
HOSTTOOLS="$ROOT/out/host/bin"      # host-side build tools we compile ourselves
# $PREFIX/bin must NOT go on PATH: our cross builds install ARM binaries there
# (glib-compile-resources, glib-compile-schemas, ...) and the build then invokes
# them on the host, failing with exit 126 "cannot execute". Only the
# arch-independent tools are linked into $HOSTTOOLS instead -- GLib ships several
# of its code generators as Python scripts (gdbus-codegen, glib-genmarshal,
# glib-mkenums), and WebKit needs gdbus-codegen because USE_ATSPI is hardcoded
# TRUE in OptionsWPE.cmake.
export PATH="$ROOT/.venv/bin:$HOSTTOOLS:$PATH"

link_portable_tools() {
    for t in gdbus-codegen glib-genmarshal glib-mkenums glib-gettextize; do
        if [ -f "$PREFIX/bin/$t" ] && head -c2 "$PREFIX/bin/$t" 2>/dev/null | grep -q "#!"; then
            ln -sf "$PREFIX/bin/$t" "$HOSTTOOLS/$t"
        fi
    done
}

mkdir -p "$PREFIX" "$TARBALLS" "$WORK" "$HOSTTOOLS"
link_portable_tools

# Verify a tarball against deps/checksums.txt, then extract it once.
# $1 = tarball filename, $2 = expected top-level directory name
unpack() {
    _tb="$TARBALLS/$1"; _dir="$WORK/$2"
    [ -f "$_tb" ] || { echo "missing tarball $1 -- run deps/fetch.sh" >&2; exit 1; }
    ( cd "$TARBALLS" && grep " $1\$" "$ROOT/deps/checksums.txt" | sha256sum -c - >/dev/null ) \
        || { echo "checksum mismatch on $1 -- refusing to build" >&2; exit 1; }
    [ -d "$_dir" ] || tar -xf "$_tb" -C "$WORK"
    echo "$_dir"
}

# Install a built package into $PREFIX by CONTENT, not by timestamp.
#
# meson/make install rewrite every file with a fresh mtime, and ninja keys on
# mtime -- so reinstalling a header whose bytes never changed cascades into
# recompiling everything that includes it. That is what turned "rebuild GLib as
# shared" (a relink, in principle) into 1148 edges of WebCore, ~40 minutes.
#
# Usage:  meson_install_by_content <build-dir>
#         auto_install_by_content  <build-dir>
# Both install to a scratch DESTDIR first, then rsync --checksum into $PREFIX,
# so only files whose contents actually differ get a new mtime.
_sync_staged() {
    _staged="$1"
    rsync -a --checksum "$_staged$PREFIX/" "$PREFIX/"
    rm -rf "$_staged"
}

meson_install_by_content() {
    _dst="$WORK/.install-staging.$$"
    rm -rf "$_dst"
    DESTDIR="$_dst" meson install -C "$1" --no-rebuild >/dev/null 2>&1 || return 1
    _sync_staged "$_dst"
}

auto_install_by_content() {
    _dst="$WORK/.install-staging.$$"
    rm -rf "$_dst"
    make -C "$1" DESTDIR="$_dst" install >/dev/null 2>&1 || return 1
    _sync_staged "$_dst"
}

# Emit a CMake toolchain file. Needed by libjpeg-turbo (3.x dropped autotools)
# and, later, by WebKit itself -- WebKit's build is CMake.
cmake_toolchain_file() {
    cat > "$WORK/n9-toolchain.cmake" <<CMTC
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER   "$CC")
set(CMAKE_CXX_COMPILER "$CXX")
set(CMAKE_SYSROOT      "$SYSROOT")
# Must be a CACHE entry: find_package(PkgConfig) calls find_program() for this
# name, and a plain normal variable makes the cache entry come out EMPTY (CMake
# policy CMP0125), so every pkg_check_modules() silently fails.
set(PKG_CONFIG_EXECUTABLE "$HOSTTOOLS/pkg-config" CACHE FILEPATH "static pkg-config wrapper" FORCE)
# Find libraries and headers in our staging prefix first, then the sysroot.
set(CMAKE_FIND_ROOT_PATH "$PREFIX;$ROOT/out/arm/icu;$SYSROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_C_FLAGS_INIT   "$CFLAGS")
set(CMAKE_CXX_FLAGS_INIT "$CXXFLAGS")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "$LDFLAGS")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "$LDFLAGS")
CMTC
    echo "$WORK/n9-toolchain.cmake"
}

# Emit the meson cross file (meson needs the toolchain described, not inherited).
meson_cross_file() {
    _name=${1:-n9-cross}
    if [ "$COMPAT_OFF" = "1" ]; then _inc=""; else _inc=", '-include', '$ROOT/deps/compat/n9-longdouble.h'"; fi
    cat > "$WORK/$_name.ini" <<CROSS
[binaries]
c          = '$CC'
cpp        = '$CXX'
ar         = '$AR'
strip      = '$STRIP'
pkg-config = 'pkg-config'

[built-in options]
c_args      = ['-O2', '-fPIC', '-D_FILE_OFFSET_BITS=64'$_inc]
cpp_args    = ['-O2', '-fPIC', '-D_FILE_OFFSET_BITS=64'$_inc]
c_link_args = [$(echo "$LDFLAGS" | sed "s/ /', '/g; s/^/'/; s/\$/'/")]

[host_machine]
system     = 'linux'
cpu_family = 'arm'
cpu        = 'armv7a'
endian     = 'little'
CROSS
    echo "$WORK/$_name.ini"
}
