#!/bin/sh
# Run the shell's logic tests on the build host, without a device.
#
# Why this exists: the shell is Qt 4.7.4 / QML1 ARM, and every one of its
# failure modes is silent -- a missing QML type resolves at creation time, an
# unregistered image provider renders nothing, a ReferenceError abandons the
# rest of a handler. `N9_TEST_QML=1` catches the first class on device; these
# catch all of them here, in a second, against the real source files.
#
# The trick is that the Qt SDK ships a DESKTOP Qt 4.7.4 with the same QML engine
# and the same com.nokia.meego components the device has. Two things stop it
# running on a modern host, and both are papered over below:
#
#   * it needs libpng12, which no current distribution ships. Nothing here
#     decodes a PNG, so a no-op library carrying the PNG12_0 version tag is
#     enough to satisfy the loader.
#   * webviewitem.cpp includes X11/EGL/GLES2. The headers come from the Madde
#     sysroot and the entry points are stubbed the same way -- the gesture code
#     under test calls none of them.
#
# /bin/sh, not zsh: zsh does not word-split $CFLAGS and friends.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QT="$HOME/QtSDK/Desktop/Qt/474/gcc"
SYS="$HOME/QtSDK/Madde/sysroots/harmattan_sysroot_10.2011.34-1_slim"
B="$ROOT/build-hosttest"

[ -d "$QT" ]  || { echo "no desktop Qt 4.7.4 at $QT" >&2; exit 1; }
[ -d "$SYS" ] || { echo "no Madde sysroot at $SYS" >&2; exit 1; }

QINC="-I$QT/include -I$QT/include/QtCore -I$QT/include/QtGui -I$QT/include/QtDeclarative"
QLIB="-L$QT/lib -lQtDeclarative -lQtGui -lQtCore"

mkdir -p "$B/lib" "$B/inc/X11/extensions" "$B/imports/com/webmee"

# --- libpng12, enough of it to load ----------------------------------------
nm -D --undefined-only "$QT/lib/libQtGui.so.4" \
    | awk '{print $NF}' | sed 's/@.*//' | grep '^png_' | sort -u > "$B/png.syms"
{
  echo '/* Generated. No-op libpng12 so the SDK Qt loads on a modern host. */'
  while read -r s; do echo "void *$s(void) { return 0; }"; done < "$B/png.syms"
} > "$B/png12.c"
{ echo 'PNG12_0 {'; echo '  global:'
  while read -r s; do echo "    $s;"; done < "$B/png.syms"
  echo '  local: *;'; echo '};'; } > "$B/png12.map"
gcc -shared -fPIC -o "$B/lib/libpng12.so.0" "$B/png12.c" \
    -Wl,--version-script="$B/png12.map"

# --- the QML import path ----------------------------------------------------
# com.webmee: a stand-in for the C++ WebViewItem, so BrowserPage compiles.
cat > "$B/imports/com/webmee/qmldir" <<'EOF'
WebView 1.0 WebView.qml
EOF
cat > "$B/imports/com/webmee/WebView.qml" <<'EOF'
import QtQuick 1.1
Item {
    property bool painted: false
    property bool crashed: false
    property int topInset: 0
    property int bottomInset: 0
    signal touched(string kind, int slot, int x, int y)
    signal surfaceSizeChanged(variant size)
}
EOF
# com.nokia.extras: take the DEVICE's copy, not the desktop SDK's -- they differ
# (InfoBanner has timerShowTime on the device and timeout on the desktop), and
# the device is what ships. Its ARM plugin cannot load here, but it only
# registers types this app does not use, and the 1.1 revisions it would declare
# are relabelled onto the .qml entries instead.
rm -rf "$B/imports/com/nokia/extras.1.1"
mkdir -p "$B/imports/com/nokia/extras.1.1"
cp "$SYS/usr/lib/qt4/imports/com/nokia/extras"/*.qml "$B/imports/com/nokia/extras.1.1/"
cp "$SYS/usr/lib/qt4/imports/com/nokia/extras"/*.js  "$B/imports/com/nokia/extras.1.1/" 2>/dev/null || true
grep -v '^plugin ' "$SYS/usr/lib/qt4/imports/com/nokia/extras/qmldir" \
    | sed 's/ 1\.0 / 1.1 /' > "$B/imports/com/nokia/extras.1.1/qmldir"

# --- webviewitem, for the gesture test --------------------------------------
rm -rf "$B/inc/EGL" "$B/inc/GLES2" "$B/inc/KHR"
cp -r "$SYS/usr/include/EGL" "$SYS/usr/include/GLES2" "$SYS/usr/include/KHR" "$B/inc/"
cp "$SYS/usr/include/X11/extensions"/*.h "$B/inc/X11/extensions/"

"$QT/bin/moc" "$ROOT/shell/webviewitem.h" -o "$B/moc_webviewitem.cpp"
g++ -w -c -o "$B/webviewitem.o" "$ROOT/shell/webviewitem.cpp" \
    $QINC -I"$B/inc" -I"$ROOT/shell"
g++ -w -c -o "$B/moc_webviewitem.o" "$B/moc_webviewitem.cpp" \
    $QINC -I"$B/inc" -I"$ROOT/shell"

nm -C --undefined-only "$B/webviewitem.o" | awk '{print $NF}' \
    | grep -E '^(gl|egl|X)[A-Za-z0-9_]*$' | sort -u > "$B/gl.syms"
{ echo '/* Generated. No-op GL/EGL/X11: the gesture code calls none of them. */'
  while read -r s; do echo "void *$s(void) { return 0; }"; done < "$B/gl.syms"
} > "$B/glstub.c"
gcc -shared -fPIC -o "$B/lib/libn9gl.so" "$B/glstub.c"

# --- build ------------------------------------------------------------------
HERE=$(cd "$(dirname "$0")" && pwd)
LINK="-L$B/lib -Wl,--allow-shlib-undefined -Wl,-rpath-link,$B/lib"
for t in qmlcheck tabtest authtest gesturetest; do
    "$QT/bin/moc" "$HERE/$t.cpp" -o "$B/$t.moc" 2>/dev/null || : > "$B/$t.moc"
done
g++ -w -o "$B/qmlcheck"    "$HERE/qmlcheck.cpp"    $QINC -I"$B" $QLIB $LINK
g++ -w -o "$B/tabtest"     "$HERE/tabtest.cpp"     $QINC -I"$B" $QLIB $LINK
g++ -w -o "$B/authtest"    "$HERE/authtest.cpp"    $QINC -I"$B" $QLIB $LINK
g++ -w -o "$B/gesturetest" "$HERE/gesturetest.cpp" "$B/webviewitem.o" "$B/moc_webviewitem.o" \
    $QINC -I"$B/inc" -I"$ROOT/shell" -I"$B" $QLIB -ln9gl $LINK

# --- run --------------------------------------------------------------------
# The theme provider and the context properties this does not stub produce a
# lot of noise; only the test lines matter.
export LD_LIBRARY_PATH="$QT/lib:$B/lib"
IMPORTS="-I $B/imports -I $QT/imports"
cd "$ROOT/shell/qml"
rc=0
echo "== every page compiles =="
"$B/qmlcheck" $IMPORTS ./*.qml 2>/dev/null || rc=1
echo
echo "== tabs =="
"$B/tabtest" main.qml "$B/imports" "$QT/imports" 2>/dev/null | grep -E '^(ok|FAIL|     )|failure' || rc=1
echo
echo "== http auth and the password store =="
"$B/authtest" BrowserPage.qml "$B/imports" "$QT/imports" 2>/dev/null | grep -E '^(ok|FAIL)|failure' || rc=1
echo
echo "== touch, pinch and panning =="
"$B/gesturetest" 2>/dev/null | grep -E '^(ok|FAIL|     )|failure' || rc=1
exit $rc
