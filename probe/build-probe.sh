#!/bin/sh
# Build the device canary. Run deps/build-icu.sh first.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLCHAIN=${TOOLCHAIN:-$HOME/cross/arm-harmattan}
TRIPLE=arm-none-linux-gnueabi
ICU="$ROOT/out/arm/icu"
OUT="$ROOT/out/arm/n9probe"

[ -f "$ICU/lib/libicuuc.a" ] || { echo "ICU not built yet -- run deps/build-icu.sh" >&2; exit 1; }

# -lpthread/-lrt are separate libraries on glibc 2.10 (they were not folded into
# libc until 2.34), and shm_open lives in librt there.
# Static libstdc++/libgcc + the loader pin are the same contract as the ICU build.
"$TOOLCHAIN/bin/$TRIPLE-g++" -O2 -std=c++17 \
    -o "$OUT" "$ROOT/probe/probe.cpp" \
    -I"$ICU/include" \
    -static-libstdc++ -static-libgcc \
    -Wl,--dynamic-linker,/lib/ld-linux.so.3 \
    -L"$ICU/lib" -licui18n -licuuc -licudata \
    -lpthread -lrt -ldl -lm

"$TOOLCHAIN/bin/$TRIPLE-strip" "$OUT"
echo "built: $OUT"
ls -lh "$OUT" | awk '{print "size:  " $5}'
echo
echo "interpreter / needed libs (must NOT list libstdc++):"
"$TOOLCHAIN/bin/$TRIPLE-readelf" -d "$OUT" | grep -E "NEEDED|RPATH|RUNPATH" || true
"$TOOLCHAIN/bin/$TRIPLE-readelf" -l "$OUT" | grep -A1 INTERP | tail -1
echo
echo "next:  scp $OUT user@n9:  &&  ssh user@n9 ./n9probe"
