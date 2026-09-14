#!/bin/sh
# Build the GLib canary. Run deps/build-glib.sh first.
. "$(dirname "$0")/../deps/toolchain.sh"
OUT="$ROOT/out/arm/n9glibprobe"
FLAGS=$(pkg-config --static --cflags --libs gio-2.0) || exit 1
# --start-group: static GLib/GObject/GIO have circular references between them
$CC -O2 "$ROOT/probe/glibprobe.c" -o "$OUT" $CFLAGS $LDFLAGS \
    $(echo "$FLAGS" | tr ' ' '\n' | grep -v '^-l' | tr '\n' ' ') \
    -Wl,--start-group $(echo "$FLAGS" | tr ' ' '\n' | grep '^-l' | tr '\n' ' ') -Wl,--end-group
$STRIP "$OUT"
echo "built: $OUT  ($(ls -lh "$OUT" | awk '{print $5}'))"
