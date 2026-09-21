#!/bin/sh
# Assemble and push the on-device tree.
#
# Target defaults to /home/user/n9browser, which `user` can write without root.
# /opt/n9browser is the Harmattan convention and is what the .deb will use, but
# /opt is root-owned and `devel-su` prompts for a password with no TTY over ssh,
# which simply hangs the deploy.
#
# Two env vars make a non-/opt location work at runtime:
#   LD_LIBRARY_PATH   binaries carry DT_RPATH=/opt/n9browser/lib; when that
#                     directory has no match, ld.so falls through to this
#   WEBKIT_EXEC_PATH  WebKit bakes in the libexec path for its auxiliary
#                     processes at build time; this overrides it
set -eu
ROOT=$(cd "$(dirname "$0")" && pwd)
# Where WebKit was INSTALLED at build time -- its configured prefix, which
# is not where it runs. The runtime prefix is /opt/WebMee.
STAGE="$ROOT/out/arm/stage/opt/n9browser"
HOST="${HOST:-user@n9}"
DEST="${DEST:-/opt/n9browser}"
STRIP="$HOME/cross/arm-harmattan/bin/arm-none-linux-gnueabi-strip"

PKG="${PKG:-$ROOT/out/arm/pkg}"
rm -rf "$PKG"
mkdir -p "$PKG/bin" "$PKG/lib" "$PKG/lib/gio/modules" "$PKG/libexec/wpe-webkit-2.0" \
         "$PKG/etc/fonts" "$PKG/etc/ssl" "$PKG/share/webmee"

cp -a "$STAGE/lib/"libWPEWebKit-2.0.so* "$PKG/lib/"
# Keep the wpe-webkit-2.0/ level. WebKit dlopen()s the injected bundle by an
# ABSOLUTE path compiled in as <libdir>/wpe-webkit-2.0/injected-bundle/, so
# flattening this directory leaves every web process starting without its
# bundle -- which shows up much later as an unexplained "web process crashed"
# on heavy pages, not as a load error.
mkdir -p "$PKG/lib/wpe-webkit-2.0"
cp -a "$STAGE/lib/wpe-webkit-2.0/." "$PKG/lib/wpe-webkit-2.0/"
# Every shared dependency we cross-built. This must be a wildcard, not a list:
# GLib, GObject, GIO, GModule and libsoup only became shared when the duplicate
# global-state bug was fixed, and a hand-written list silently went stale --
# the engine died with "libsoup-3.0.so.0: cannot open shared object file".
find "$ROOT/out/arm/sysroot/lib" -maxdepth 1 -name "*.so*" -type f -exec cp -a {} "$PKG/lib/" \;
find "$ROOT/out/arm/sysroot/lib" -maxdepth 1 -name "*.so*" -type l -exec cp -a {} "$PKG/lib/" \;

# Toolchain runtime the device cannot provide: its libstdc++ is GCC 4.x and it
# has no libatomic at all.
cp -a "$ROOT/out/arm/runtime/libstdc++.so.6" "$PKG/lib/"
cp -a "$ROOT/out/arm/runtime/libatomic.so.1" "$PKG/lib/"

# TLS. glib-networking is a runtime GIO module -- nothing links it, GIO
# dlopen()s it -- so it must be shipped as its own file or https:// simply
# fails with "TLS support is not available" and nothing at build time says so.
cp -a "$ROOT/out/arm/sysroot/lib/gio/modules/"*.so "$PKG/lib/gio/modules/" 2>/dev/null || true

# The device's own CA store is from 2011 and predates most roots in use today
# (ISRG Root X1, which every Let's Encrypt site chains to, is from 2015), so
# trusting it would fail on a large part of the modern web. Ship a current
# bundle and point OpenSSL at it explicitly.
if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
    cp -a /etc/ssl/certs/ca-certificates.crt "$PKG/etc/ssl/ca-certificates.crt"
else
    echo "warning: no host CA bundle at /etc/ssl/certs/ca-certificates.crt; HTTPS will not verify" >&2
fi

cp -a "$STAGE/libexec/wpe-webkit-2.0/"* "$PKG/libexec/wpe-webkit-2.0/"
cp -a "$ROOT/out/arm/webmee-engine" "$PKG/bin/"
# ARM binary, useless on the build host, essential on the device: it is what
# writes giomodule.cache after the tree lands.
cp -a "$ROOT/out/arm/sysroot/bin/gio-querymodules" "$PKG/bin/" 2>/dev/null || true
cp -a "$ROOT/deps/compat/fonts/fonts.conf" "$PKG/etc/fonts/"
# The content blocker's ruleset. WebKit compiles it into a DFA on first use and
# caches that under ~/.cache/WebMee/filters, keyed by a FIXED identifier -- so a
# changed ruleset would otherwise keep matching against the old compiled copy.
# Shipping a new list therefore has to drop the cache; the engine recompiles in
# about 70 ms, measured on device.
cp -a "$ROOT/app/blocklist.json" "$PKG/share/webmee/"

# libepoxy resolves GL at runtime by dlopen()ing the STANDARD sonames
# (libEGL.so.1, libGLESv2.so.2), but the PowerVR driver ships only unversioned
# libEGL.so -> libEGL_r125.so. Without these the engine loads, starts painting,
# and then dies with "Couldn't open libEGL.so.1". The links live in our own lib
# directory rather than /usr/lib, so nothing system-wide is touched.
ln -sf /usr/lib/libEGL.so    "$PKG/lib/libEGL.so.1"
ln -sf /usr/lib/libGLESv2.so "$PKG/lib/libGLESv2.so.2"
ln -sf /usr/lib/libGLESv2.so "$PKG/lib/libGLESv2.so.1"

# Fail loudly rather than ship a tree that only misbehaves at runtime.
for required in \
    "lib/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so" \
    "libexec/wpe-webkit-2.0/WPEWebProcess" \
    "libexec/wpe-webkit-2.0/WPENetworkProcess" \
    "lib/gio/modules/libgioopenssl.so" \
    "bin/webmee-engine"; do
    [ -e "$PKG/$required" ] || { echo "missing from package: $required" >&2; exit 1; }
done

# Parallel strip: the unstripped tree is ~130 MB and the eMMC write is the
# bottleneck, so this pays for itself several times over.
find "$PKG" -type f \( -name "*.so*" -o -perm -u+x \) -print0 \
  | xargs -0 -P 4 -n 8 "$STRIP" --strip-unneeded 2>/dev/null || true
echo "package: $(du -sh "$PKG" | cut -f1)"

# STAGE_ONLY builds the tree without touching the device -- that is how
# package.sh gets the payload for the .deb.
if [ -n "${STAGE_ONLY:-}" ]; then
    echo "staged at $PKG"
    exit 0
fi

ssh -o BatchMode=yes "$HOST" "killall webmee-engine n9engine WPEWebProcess WPENetworkProcess 2>/dev/null; rm -rf $DEST ~/.cache/WebMee/filters; mkdir -p $DEST"
tar -C "$PKG" -cf - . | ssh -o BatchMode=yes "$HOST" "tar -C $DEST -xf -"

# giomodule.cache has to be built by an ARM binary, so it is built where ARM
# binaries run. Without it GIO still works -- it just loads every module in the
# directory rather than consulting the cache.
ssh -o BatchMode=yes "$HOST" \
  "LD_LIBRARY_PATH=$DEST/lib $DEST/bin/gio-querymodules $DEST/lib/gio/modules 2>/dev/null \
   && echo 'giomodule.cache built' || echo 'no gio-querymodules on device (harmless)'"
echo "deployed to $HOST:$DEST"
