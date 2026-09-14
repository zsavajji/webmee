#!/bin/sh
# Dependency policy audit. Run BEFORE building WebKit -- it takes seconds and
# catches the class of mistake that otherwise only surfaces at the final link or
# at runtime, after hours of compiling.
#
# Two rules, both learned the expensive way:
#
# 1. Every static archive must be PIC. A non-PIC .a links fine into executables
#    (which is what every probe was) and fails only when something finally tries
#    to build a shared object -- here, libWPEWebKit.so, at edge 388 of 397.
#
# 2. Any library holding PROCESS-GLOBAL state must be shared, never static.
#    Linked statically it ends up duplicated: once inside libWPEWebKit.so (where
#    -fvisibility=hidden makes it invisible) and once in the application. The two
#    copies never see each other. Observed twice:
#      - libwpe   : app's wpe_view_backend was not the one WebKit rendered into
#      - GObject  : "signal 'load-changed' is invalid for instance ... of type
#                   '(NULL)'" because the GType registries were separate
#    Leaf libraries with no shared registry (ICU, Cairo, HarfBuzz, ...) are fine
#    static and stay that way.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX="$ROOT/out/arm/sysroot"
ICU="$ROOT/out/arm/icu"
TC="$HOME/cross/arm-harmattan/bin/arm-none-linux-gnueabi"
fail=0

# Libraries that MUST be shared, and why.
must_be_shared="glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0 gthread-2.0 wpe-1.0 soup-3.0"

echo "== 1. static archives must be PIC =="
for a in "$PREFIX"/lib/*.a "$ICU"/lib/*.a; do
    [ -f "$a" ] || continue
    n=$("$TC-readelf" -r "$a" 2>/dev/null | grep -c "R_ARM_MOVW_ABS_NC" || true)
    if [ "$n" -gt 0 ]; then
        echo "  NOT PIC: $(basename "$a")  ($n absolute relocations)"
        fail=1
    fi
done
[ "$fail" -eq 0 ] && echo "  all PIC"

echo "== 2. process-global libraries must be shared =="
for lib in $must_be_shared; do
    if [ -f "$PREFIX/lib/lib$lib.so" ]; then
        echo "  shared  lib$lib"
    elif [ -f "$PREFIX/lib/lib$lib.a" ]; then
        echo "  STATIC  lib$lib  <-- must be shared (duplicate global state)"
        fail=1
    else
        echo "  absent  lib$lib"
    fi
done

echo "== 3. GObject type registry must have exactly one home =="
hits=0
for so in "$PREFIX"/lib/libgobject-2.0.so*; do
    [ -f "$so" ] || continue
    if "$TC-nm" -D --defined-only "$so" 2>/dev/null | grep -q " T g_type_register_static"; then
        hits=$((hits + 1))
    fi
done
if [ "$hits" -eq 0 ]; then
    echo "  no shared libgobject exporting g_type_register_static"
    fail=1
else
    echo "  libgobject exports the registry"
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "FAIL -- fix before building WebKit; a rebuild here costs ~2 hours."
    exit 1
fi
echo "OK"
