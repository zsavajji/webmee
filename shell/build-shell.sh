#!/bin/sh
# Build the Qt shell with the SDK's original toolchain (GCC 4.4 + Qt 4.7.4).
# This is intentionally NOT the GCC 14 cross toolchain used for the engine: the
# shell links the device's Qt, so it must match that C++ ABI.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
MADDE="$HOME/QtSDK/Madde"
MAD="$MADDE/bin/mad"

# Must go through `mad`. Calling the target's qmake directly leaves the host
# compiler in place -- it then tries to assemble Qt's ARM inline asm
# (qatomic_armv6.h) as x86_64 and fails. `mad` sets the cross toolchain, sysroot
# and mkspec for the selected target.
[ -x "$MAD" ] || { echo "no madde wrapper at $MAD" >&2; exit 1; }

B="$HERE/../build-shell"
mkdir -p "$B"
( cd "$B" && "$MAD" qmake "$HERE/webmee.pro" && "$MAD" make -j"${JOBS:-3}" )
echo
echo "built: $B/WebMee"
