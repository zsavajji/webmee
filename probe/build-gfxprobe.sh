#!/bin/sh
. "$(dirname "$0")/../deps/toolchain.sh"
OUT="$ROOT/out/arm/n9gfxprobe"
FLAGS=$(pkg-config --static --cflags --libs cairo harfbuzz fontconfig freetype2) || exit 1
$CC -O2 "$ROOT/probe/gfxprobe.c" -o "$OUT" $CFLAGS $CPPFLAGS $LDFLAGS \
    $(echo "$FLAGS" | tr ' ' '\n' | grep -v '^-l' | tr '\n' ' ') \
    -Wl,--start-group $(echo "$FLAGS" | tr ' ' '\n' | grep '^-l' | tr '\n' ' ') -lstdc++ -Wl,--end-group
$STRIP "$OUT"
echo "built: $OUT ($(ls -lh "$OUT" | awk '{print $5}'))"
