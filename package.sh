#!/bin/sh
# Build the installable WebMee .deb.
#
# Assembles the same tree deploy.sh pushes over ssh (STAGE_ONLY=1 stops it
# before it touches the device), adds the desktop entry and icon, and wraps the
# lot with dpkg-deb.
#
# -Zgzip is NOT optional: Harmattan's dpkg is from 2011 and cannot read the
# zstd/xz compression modern dpkg-deb defaults to. A package built without it
# installs fine on a desktop and fails on the device with "unsupported
# compression".
set -eu
ROOT=$(cd "$(dirname "$0")" && pwd)
VERSION="${VERSION:-0.1}"
ARCH=armel
DEB="$ROOT/out/arm/deb"
OUT="$ROOT/out/WebMee_${VERSION}_${ARCH}.deb"

command -v dpkg-deb >/dev/null || { echo "dpkg-deb not found" >&2; exit 1; }

echo "==> building"
"$ROOT/app/build-app.sh"    >/dev/null
"$ROOT/backend/build-backend.sh" >/dev/null
"$ROOT/shell/build-shell.sh" 2>&1 | grep -iE "error|warning:" || true
[ -x "$ROOT/build-shell/WebMee" ] || { echo "shell did not build" >&2; exit 1; }

echo "==> staging payload"
rm -rf "$DEB"
# The payload directory stays /opt/n9browser: WebKit compiled that path in as
# PKGLIBEXECDIR and a release build has no runtime override for it. Only the
# app is WebMee.
mkdir -p "$DEB/DEBIAN" "$DEB/opt/n9browser" \
         "$DEB/usr/share/applications" \
         "$DEB/usr/share/icons/hicolor/80x80/apps"
PKG="$DEB/opt/n9browser" STAGE_ONLY=1 "$ROOT/deploy.sh" >/dev/null

cp -a "$ROOT/build-shell/WebMee"        "$DEB/opt/n9browser/bin/WebMee"
cp -a "$ROOT/shell/webmee.desktop"      "$DEB/usr/share/applications/"
cp -a "$ROOT/icon/webmee80.png"         "$DEB/usr/share/icons/hicolor/80x80/apps/"
"$HOME/cross/arm-harmattan/bin/arm-none-linux-gnueabi-strip" \
    --strip-unneeded "$DEB/opt/n9browser/bin/WebMee" 2>/dev/null || true

SIZE=$(du -sk "$DEB" | cut -f1)

# Depends are the real Harmattan package names (checked with dpkg -S on device).
# Everything else the engine needs -- WebKit, GLib, libsoup, OpenSSL, Cairo,
# HarfBuzz, the GCC 14 libstdc++ -- is bundled under /opt/n9browser, because none
# of it exists on a 2011 device at a usable version.
cat > "$DEB/DEBIAN/control" <<CTRL
Package: webmee
Version: $VERSION
Architecture: $ARCH
Section: user/network
Priority: optional
Maintainer: WebMee <blackbird5308@gmail.com>
Installed-Size: $SIZE
Depends: libqt4-declarative, libqt4-opengl, libxcomposite1, libsignon-qt0, applauncherd-invoker, applauncherd-launcher
Description: WebMee - a modern web engine for the N9
 WPE WebKit 2.44 with JIT and GPU compositing, wrapped in the original
 Harmattan browser interface. Renders contemporary layouts (flexbox, CSS
 grid, custom properties) that the stock 2011 browser cannot.
CTRL

# giomodule.cache must be written by an ARM binary, so it is written here rather
# than at build time. Without it GIO still finds the TLS backend -- it just
# loads every module in the directory instead of consulting the cache.
cat > "$DEB/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
if [ "$1" = configure ] && [ -x /opt/n9browser/bin/gio-querymodules ]; then
    LD_LIBRARY_PATH=/opt/n9browser/lib \
        /opt/n9browser/bin/gio-querymodules /opt/n9browser/lib/gio/modules || true
fi
exit 0
POST
chmod 755 "$DEB/DEBIAN/postinst"

echo "==> packing"
rm -f "$OUT"
fakeroot dpkg-deb -Zgzip --build "$DEB" "$OUT" >/dev/null
echo "built: $OUT ($(du -h "$OUT" | cut -f1), installs $((SIZE/1024)) MB)"
