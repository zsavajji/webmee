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
VERSION="${VERSION:-0.3.1}"
ARCH=armel
DEB="$ROOT/out/arm/webmee"   # last component is the package name: gendigsigsums reads it
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

# md5sums. Every real .deb carries one, and this package had none -- only
# control and postinst. On Harmattan it is also the most likely thing aegis
# correlates the manifest's <for path> entries against: without it the install
# says
#     <for> refers to '/opt/n9browser/bin/WebMee', not installed by that
#     package -- ignored
# and the manifest grants nothing. (Unconfirmed as the cause -- aegis-deb-add
# only appends to the ar archive and validates nothing, so this could not be
# checked off-device. Shipping it is correct packaging either way.)
( cd "$DEB" && find . -path ./DEBIAN -prune -o -type f -print0 \
    | xargs -0 md5sum | sed 's|  \./|  |' > DEBIAN/md5sums )

# digsigsums: the aegis per-file hash list. Without it the install says
#     file /var/lib/dpkg/tmp.ci/digsigsums cannot be opened
#     No digsigsums file
# and, because aegis correlates the manifest against it, also
#     <for> refers to '/opt/n9browser/bin/WebMee', not installed by that package
# Madde's own dpkg-deb wrapper generates this; we call the host dpkg-deb, so we
# have to run the same tool ourselves. It refuses to overwrite, hence the rm.
GENSUMS="$HOME/QtSDK/Madde/madbin/gendigsigsums"
if [ -x "$GENSUMS" ]; then
    rm -f "$DEB/DEBIAN/digsigsums"
    "$GENSUMS" -o com.nokia.maemo "$DEB/DEBIAN/digsigsums" "$DEB" \
        && echo "     digsigsums generated ($(grep -c "" "$DEB/DEBIAN/digsigsums") files)" \
        || echo "     WARNING: gendigsigsums failed; aegis will ignore the manifest" >&2
else
    echo "     WARNING: no gendigsigsums at $GENSUMS" >&2
fi

echo "==> packing"
rm -f "$OUT"
fakeroot dpkg-deb -Zgzip --build "$DEB" "$OUT" >/dev/null

# The aegis manifest ships as a TOP-LEVEL ar member named _aegis, not as a file
# inside control.tar.gz -- which is why `dpkg-deb -I` does not list it and
# `ar t` does. `mad dpkg-buildpackage` would attach it by itself; this build
# does not go through debhelper, so attach it directly with the same tool.
AEGIS="$ROOT/debian/webmee.aegis"
ADD="$HOME/QtSDK/Madde/madbin/aegis-deb-add"
# Validate before attaching. aegis parses the manifest at INSTALL time on the
# device, and rejects the whole package when it fails -- so a malformed one is
# only discovered after a 50 MB transfer. Its diagnostics are misleading too: a
# double hyphen inside an XML comment is illegal XML, and aegis reports that as
# "Manifest cannot have <! ..> elements", which reads like a ban on comments.
if [ -f "$AEGIS" ]; then
    python3 -c "import sys,xml.etree.ElementTree as E; E.parse(sys.argv[1])" "$AEGIS"         || { echo "aegis manifest is not well-formed XML; refusing to package" >&2; exit 1; }
fi
if [ -f "$AEGIS" ] && [ -x "$ADD" ]; then
    "$ADD" "$OUT" "$AEGIS=_aegis" && echo "     aegis manifest attached"
else
    echo "     WARNING: no aegis manifest attached (missing $AEGIS or $ADD)" >&2
fi
echo "built: $OUT ($(du -h "$OUT" | cut -f1), installs $((SIZE/1024)) MB)"
