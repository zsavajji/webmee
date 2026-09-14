#!/bin/sh
# Build the engine process. /bin/sh for the word-splitting reason in
# backend/build-backend.sh.
. "$(dirname "$0")/../deps/toolchain.sh"

# WebKit is installed under DESTDIR, so its .pc still says prefix=/opt/n9browser
# -- the build-time prefix. The runtime prefix is /opt/WebMee, reached via
# WEBKIT_EXEC_PATH and LD_LIBRARY_PATH rather than by rebuilding WebKit.
# (the DEVICE path). PKG_CONFIG_SYSROOT_DIR would fix that but would also get
# prepended to every other package, whose prefixes are already host paths -- so
# point at the staged WebKit directly and use pkg-config only for the rest.
STAGE="$ROOT/out/arm/stage$DEVPREFIX"
WK_CFLAGS="-I$STAGE/include/wpe-webkit-2.0"
WK_LIBS="-L$STAGE/lib -lWPEWebKit-2.0"
# -L out/arm/runtime: libWPEWebKit needs libatomic.so.1, which lives in the
# toolchain rather than the device sysroot, and is bundled at install time.

GLIB_FLAGS=$(pkg-config --cflags --libs glib-2.0 gobject-2.0 libsoup-3.0 wpe-1.0) || exit 1
OUT="$ROOT/out/arm/webmee-engine"

$CC -std=gnu99 -O2 "$ROOT/app/n9engine.c" -o "$OUT" \
    $CFLAGS $CPPFLAGS $WK_CFLAGS \
    $LDFLAGS -L"$ROOT/out/arm/runtime" $WK_LIBS $GLIB_FLAGS || exit 1

echo "built $OUT"
ls -lh "$OUT" | awk '{print "size:", $5}'
