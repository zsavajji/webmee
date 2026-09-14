#!/bin/sh
# Build the n9browser WPE backend as a loadable module, libWPEBackend-n9.so.
#
# A shared object, not a static lib: libwpe is now shared and uses its dlopen
# loader (src/loader.c), which looks up _wpe_loader_interface with dlsym in the
# backend named by WPE_BACKEND, or by the default-backend baked in at build time
# (we set libWPEBackend-n9.so).
#
# Must be /bin/sh: zsh does not word-split unquoted parameter expansions, so
# "$CFLAGS" would arrive at gcc as a single argument.
. "$(dirname "$0")/../deps/toolchain.sh"

SRC="$ROOT/backend/n9-wpe-backend.c"
OUT="$PREFIX/lib/libWPEBackend-n9.so"

WPE_CFLAGS=$(pkg-config --cflags wpe-1.0) || exit 1

rm -f "$OUT" "$PREFIX/lib/libn9wpebackend.a"
# -funwind-tables and --export-dynamic are for the crash handler: without
# them backtrace() cannot unwind out of this module and prints one frame.
$CC -std=gnu99 -O2 -fPIC -shared -funwind-tables -rdynamic -o "$OUT" "$SRC" \
    $CFLAGS $CPPFLAGS $WPE_CFLAGS \
    $LDFLAGS -lwpe-1.0 -lX11 -lXcomposite -lEGL || exit 1

echo "built $OUT"
$NM -D --defined-only "$OUT" | grep _wpe_loader_interface
