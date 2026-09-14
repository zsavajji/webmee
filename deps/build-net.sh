#!/bin/sh
# The networking tier: OpenSSL 3, libpsl, glib-networking, libsoup.
#
# TLS backend choice: glib-networking can sit on GnuTLS or OpenSSL. OpenSSL is
# picked because the GnuTLS route drags in gmp + nettle + libtasn1 +
# libunistring + p11-kit, and because OpenSSL 3 on armv7 for this device is
# already proven ground (MeeTube ships it). libatomic is linked statically from
# the toolchain, which avoids MeeTube's need to bundle libatomic.so.1 --
# GCC-14's armv7 OpenSSL emits __atomic_* calls the 2011 sysroot cannot satisfy.
. "$(dirname "$0")/toolchain.sh"

# ---- OpenSSL 3 --------------------------------------------------------------
if [ ! -f "$PREFIX/lib/libssl.a" ]; then
    echo "==> openssl-3.0.13"
    d=$(unpack openssl-3.0.13.tar.gz openssl-3.0.13)
    b="$WORK/openssl-build"; rm -rf "$b"; mkdir -p "$b"
    # openssldir is where it looks for certs ON DEVICE, so it is the install
    # prefix, not the staging dir.
    ( cd "$b" && CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" \
      "$d/Configure" linux-armv4 \
        --prefix="$PREFIX" --openssldir="$DEVPREFIX/etc/ssl" \
        no-shared no-tests no-legacy \
        $CFLAGS -latomic \
        >config.log 2>&1 || { echo "--- configure failed ---"; tail -30 config.log; exit 1; } )
    make -C "$b" -j"$JOBS" >"$b/make.log" 2>&1 || { echo "--- build failed ---"; tail -40 "$b/make.log"; exit 1; }
    make -C "$b" install_sw >/dev/null 2>&1
fi

# ---- libpsl (public suffix list; libsoup needs it for cookie policy) --------
# runtime=no, i.e. no IDNA backend. With runtime=libicu, libpsl pulls STATIC ICU
# -- a C++ library -- into libsoup, and the resulting C shared object ends up
# with unresolved std::condition_variable symbols. The cost is that libpsl will
# not punycode-convert internationalised domains itself; WebKit already
# normalises IDN via its own ICU before cookie-domain checks reach libpsl.
if [ ! -f "$PREFIX/lib/libpsl.a" ]; then
    echo "==> libpsl-0.21.5"
    d=$(unpack libpsl-0.21.5.tar.gz libpsl-0.21.5)
    CROSS=$(meson_cross_file)
    b="$WORK/libpsl-build"; rm -rf "$b"
    meson setup "$b" "$d" --cross-file "$CROSS" --prefix="$PREFIX" \
        --default-library=static --buildtype=release -Dprefer_static=true \
        -Druntime=no -Dbuiltin=true -Dtests=false \
        >"$WORK/libpsl-setup.log" 2>&1 || { echo "--- meson setup failed ---"; tail -25 "$WORK/libpsl-setup.log"; exit 1; }
    meson compile -C "$b" -j "$JOBS" >"$b/compile.log" 2>&1 \
        || { echo "--- compile failed ---"; tail -30 "$b/compile.log"; exit 1; }
    meson install -C "$b" --no-rebuild >/dev/null 2>&1
fi

# ---- nghttp2 ----------------------------------------------------------------
# libsoup 3 declares dependency('libnghttp2') with no required:false, so HTTP/2
# is not optional here. Library only -- we do not want its tools or Python glue.
if [ ! -f "$PREFIX/lib/libnghttp2.a" ]; then
    echo "==> nghttp2-1.64.0"
    d=$(unpack nghttp2-1.64.0.tar.xz nghttp2-1.64.0)
    b="$WORK/nghttp2-build"; rm -rf "$b"; mkdir -p "$b"
    ( cd "$b" && "$d/configure" --host=$TRIPLE --prefix="$PREFIX" \
        --enable-static --disable-shared --enable-lib-only \
        >config.log 2>&1 || { echo "--- configure failed ---"; tail -30 config.log; exit 1; } )
    make -C "$b" -j"$JOBS" >"$b/make.log" 2>&1 || { echo "--- build failed ---"; tail -30 "$b/make.log"; exit 1; }
    make -C "$b" install >/dev/null 2>&1
fi

# ---- libsoup ----------------------------------------------------------------
# tls_check=false is deliberate, and upstream sanctions it: glib-networking is a
# RUNTIME GIO module, not a compile-time dependency. Deferring it keeps the
# critical path aimed at WebKit -- which needs libsoup merely to configure --
# and the TLS backend can be added before we need HTTPS. Local-file rendering,
# the milestone that actually proves flexbox/grid, needs no network at all.
if [ ! -f "$PREFIX/lib/libsoup-3.0.a" ]; then
    echo "==> libsoup-3.4.4"
    d=$(unpack libsoup-3.4.4.tar.xz libsoup-3.4.4)
    CROSS=$(meson_cross_file)
    b="$WORK/libsoup-build"; rm -rf "$b"
    # SHARED: libsoup is GObject-based (SoupSession, SoupCookie, SoupMessage are
    # GTypes) and WebKit's public API exposes Soup types. Static would give
    # libWPEWebKit.so and the application separate GType registrations for them --
    # the same duplicate-global-state failure already hit with GLib and libwpe.
    # NOT -Dprefer_static: with our GLib now shared, preferring static archives
    # makes meson fall through to the DEVICE sysroot's libglib-2.0.a -- GLib 2.28
    # from 2011, non-PIC, which fails with "unresolvable R_ARM_JUMP24 relocation".
    # The pkg-config wrapper already passes --static, so Libs.private still
    # resolves without it.
    meson setup "$b" "$d" --cross-file "$CROSS" --prefix="$PREFIX" \
        --default-library=shared --buildtype=release \
        -Dtls_check=false -Dtests=false -Dintrospection=disabled -Dvapi=disabled \
        -Ddocs=disabled -Dsysprof=disabled -Dbrotli=disabled \
        -Dntlm=disabled -Dgssapi=disabled \
        >"$WORK/libsoup-setup.log" 2>&1 || { echo "--- meson setup failed ---"; tail -25 "$WORK/libsoup-setup.log"; exit 1; }
    # The library is what we want; libsoup's example programs fail to link
    # because they use the gcc driver with no -lstdc++ while libsoup pulls in a
    # C++ symbol transitively. They are not installed, so build the library
    # target and let the examples fail.
    meson compile -C "$b" -j "$JOBS" >"$b/compile.log" 2>&1 || true
    if [ ! -f "$b/libsoup/libsoup-3.0.so.0.7.1" ]; then
        echo "--- libsoup library itself failed to build ---"
        tail -30 "$b/compile.log"; exit 1
    fi
    meson_install_by_content "$b" || true
    # install stops at the first failing example, so place the library by hand.
    # -maxdepth/-type f: the bare glob also matches libsoup-3.0.so.0.7.1.p,
    # meson's object directory, which must not land in the prefix.
    find "$b/libsoup" -maxdepth 1 -type f -name "libsoup-3.0.so*" -exec cp -a {} "$PREFIX/lib/" \; 2>/dev/null || true
fi

echo
echo "==> networking tier installed"
ls "$PREFIX/lib" | grep -E "libssl|libcrypto|libpsl|libnghttp2|libsoup" | tr '\n' ' '; echo

# ---- glib-networking (TLS) --------------------------------------------------
# The GIO module that makes HTTPS exist. libsoup was configured with
# tls_check=false, which is legal precisely because this is a RUNTIME module:
# nothing links it, GIO dlopen()s it out of gio/modules and libsoup then finds a
# GTlsBackend. Without it every https:// URL fails with "TLS support is not
# available" and no build-time error ever says so.
#
# OpenSSL, not GnuTLS -- see the header of this file. Upstream discourages the
# OpenSSL backend for general distros, but the GnuTLS chain (gmp, nettle,
# libtasn1, libunistring, p11-kit) is five more cross builds for a device that
# already has proven OpenSSL 3 armv7 support.
#
# SHARED, and no -Dprefer_static: the same two traps as libsoup. A static module
# cannot be dlopen()ed, and preferring static archives makes meson fall through
# to the device sysroot's 2011 GLib.
if [ ! -f "$PREFIX/lib/gio/modules/libgioopenssl.so" ]; then
    echo "==> glib-networking-2.78.1"
    d=$(unpack glib-networking-2.78.1.tar.xz glib-networking-2.78.1)
    CROSS=$(meson_cross_file)
    b="$WORK/glib-networking-build"; rm -rf "$b"
    # libproxy and the GNOME proxy backend are desktop plumbing; environment_proxy
    # gives http_proxy/https_proxy support for free instead.
    meson setup "$b" "$d" --cross-file "$CROSS" --prefix="$PREFIX" \
        --default-library=shared --buildtype=release \
        -Dgnutls=disabled -Dopenssl=enabled \
        -Dlibproxy=disabled -Dgnome_proxy=disabled -Denvironment_proxy=enabled \
        -Dinstalled_tests=false \
        >"$WORK/glib-networking-setup.log" 2>&1 \
        || { echo "--- meson setup failed ---"; tail -25 "$WORK/glib-networking-setup.log"; exit 1; }
    meson compile -C "$b" -j "$JOBS" >"$b/compile.log" 2>&1 \
        || { echo "--- compile failed ---"; tail -40 "$b/compile.log"; exit 1; }
    # The post-install step runs gio-querymodules, which is an ARM binary and
    # cannot execute here -- the same trap as glib-compile-resources. It only
    # writes giomodule.cache, and without that cache GIO simply loads every
    # module in the directory instead of consulting it, so the modules
    # themselves are already complete. deploy.sh regenerates the cache ON THE
    # DEVICE, where the binary runs.
    meson install -C "$b" --no-rebuild >"$b/install.log" 2>&1 || true
    [ -f "$PREFIX/lib/gio/modules/libgioopenssl.so" ] \
        || { echo "--- install failed ---"; tail -20 "$b/install.log"; exit 1; }
    ls -l "$PREFIX/lib/gio/modules/"
fi
