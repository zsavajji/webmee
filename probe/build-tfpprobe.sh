#!/bin/sh
# Built with the SDK's GCC 4.4 against the Madde sysroot, because this probes
# what the SHELL will do -- and the shell is the GCC 4.4 side.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SYSROOT="$HOME/QtSDK/Madde/sysroots/harmattan_sysroot_10.2011.34-1_slim"
CC="$HOME/QtSDK/Madde/toolchains/arm-2009q3-67-arm-none-linux-gnueabi-x86_64-unknown-linux-gnu/arm-2009q3-67/bin/arm-none-linux-gnueabi-gcc"
OUT="$ROOT/out/arm/n9tfpprobe"
"$CC" --sysroot="$SYSROOT" -O2 "$ROOT/probe/tfpprobe.c" -o "$OUT" \
    -I"$SYSROOT/usr/include" \
    -L"$SYSROOT/usr/lib" \
    -lX11 -lXcomposite -lXdamage -lXext -lEGL -lGLESv2
echo "built: $OUT"
