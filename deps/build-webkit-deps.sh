#!/bin/sh
# The last tier before WebKit itself: the packages WPE's OptionsWPE.cmake marks
# REQUIRED that nothing earlier pulled in, plus WOFF2 (optional in CMake, but web
# fonts are not optional for rendering modern pages correctly).
. "$(dirname "$0")/toolchain.sh"

auto() {   # $1=guard lib  $2=tarball  $3=srcdir  then configure args
    guard="$1"; tb="$2"; sd="$3"; shift 3
    [ -f "$PREFIX/lib/$guard" ] && { echo "==> $sd (cached)"; return 0; }
    echo "==> $sd"
    d=$(unpack "$tb" "$sd")
    b="$WORK/$sd-build"; rm -rf "$b"; mkdir -p "$b"
    ( cd "$b" && "$d/configure" --host=$TRIPLE --prefix="$PREFIX" \
        --enable-static --disable-shared "$@" \
        >config.log 2>&1 || { echo "--- configure failed ---"; tail -30 config.log; exit 1; } )
    make -C "$b" -j"$JOBS" >"$b/make.log" 2>&1 || { echo "--- build failed ---"; tail -30 "$b/make.log"; exit 1; }
    make -C "$b" install >/dev/null 2>&1
}

mson() {   # $1=guard lib  $2=tarball  $3=srcdir  then meson args
    guard="$1"; tb="$2"; sd="$3"; shift 3
    [ -f "$PREFIX/lib/$guard" ] && { echo "==> $sd (cached)"; return 0; }
    echo "==> $sd"
    d=$(unpack "$tb" "$sd")
    CROSS=$(meson_cross_file)
    b="$WORK/$sd-build"; rm -rf "$b"
    meson setup "$b" "$d" --cross-file "$CROSS" --prefix="$PREFIX" \
        --default-library=static --buildtype=release -Dprefer_static=true "$@" \
        >"$WORK/$sd-setup.log" 2>&1 || { echo "--- meson setup failed ---"; tail -25 "$WORK/$sd-setup.log"; exit 1; }
    meson compile -C "$b" -j "$JOBS" >"$b/compile.log" 2>&1 \
        || { echo "--- compile failed ---"; tail -30 "$b/compile.log"; exit 1; }
    meson install -C "$b" --no-rebuild >/dev/null 2>&1
}

cmk() {    # $1=guard lib  $2=tarball  $3=srcdir  then cmake args
    guard="$1"; tb="$2"; sd="$3"; shift 3
    [ -f "$PREFIX/lib/$guard" ] && { echo "==> $sd (cached)"; return 0; }
    echo "==> $sd"
    d=$(unpack "$tb" "$sd")
    TC=$(cmake_toolchain_file)
    b="$WORK/$sd-build"; rm -rf "$b"; mkdir -p "$b"
    # CMAKE_POLICY_VERSION_MINIMUM: CMake 4 refuses projects declaring
    # cmake_minimum_required below 3.5, and woff2 1.0.2 still asks for 3.0.
    cmake -S "$d" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$TC" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF "$@" \
        >"$WORK/$sd-setup.log" 2>&1 || { echo "--- cmake failed ---"; tail -25 "$WORK/$sd-setup.log"; exit 1; }
    cmake --build "$b" -j "$JOBS" >"$b/build.log" 2>&1 \
        || { echo "--- build failed ---"; tail -30 "$b/build.log"; exit 1; }
    cmake --install "$b" >/dev/null 2>&1
}

# ---- crypto: WebCrypto and the certificate/ASN.1 paths ----------------------
auto libgpg-error.a libgpg-error-1.48.tar.bz2 libgpg-error-1.48 \
    --disable-doc --disable-tests --disable-nls
# libgcrypt has no --disable-tests (configure warns "unrecognized option"), and
# its tests/bench-slope.c fails to build here on an implicit gettimeofday. The
# library itself is fine, so recurse only into the subdirs that produce it.
if [ ! -f "$PREFIX/lib/libgcrypt.a" ]; then
    echo "==> libgcrypt-1.10.3"
    d=$(unpack libgcrypt-1.10.3.tar.bz2 libgcrypt-1.10.3)
    b="$WORK/libgcrypt-1.10.3-build"; rm -rf "$b"; mkdir -p "$b"
    ( cd "$b" && "$d/configure" --host=$TRIPLE --prefix="$PREFIX" \
        --enable-static --disable-shared --disable-doc \
        --with-libgpg-error-prefix="$PREFIX" \
        >config.log 2>&1 || { echo "--- configure failed ---"; tail -30 config.log; exit 1; } )
    make -C "$b" -j"$JOBS" SUBDIRS="compat mpi cipher random src" >"$b/make.log" 2>&1 \
        || { echo "--- build failed ---"; tail -30 "$b/make.log"; exit 1; }
    make -C "$b" SUBDIRS="compat mpi cipher random src" install >/dev/null 2>&1
fi
auto libtasn1.a libtasn1-4.19.0.tar.gz libtasn1-4.19.0 \
    --disable-doc --disable-gcc-warnings

# ---- libxkbcommon: required even headless (WebKit uses its keysym tables) ---
mson libxkbcommon.a libxkbcommon-1.6.0.tar.xz libxkbcommon-1.6.0 \
    -Denable-x11=false -Denable-wayland=false -Denable-docs=false \
    -Denable-tools=false -Denable-bash-completion=false

# ---- libepoxy: GL entry-point dispatch ------------------------------------
# Required by WPE unconditionally. It resolves GL symbols with dlopen at RUNTIME,
# so building it does not bind us to a GL driver -- which matters here, because
# the SGX530 is GLES2-only and we paint on the CPU regardless. glx/x11 off; the
# EGL and GLES2 headers come from the device sysroot.
mson libepoxy.a libepoxy-1.5.10.tar.gz libepoxy-1.5.10 \
    -Dglx=no -Dx11=false -Degl=yes -Dtests=false

# ---- brotli + WOFF2: web fonts --------------------------------------------
# CMake-optional, but a modern page whose webfont fails to decode reflows with
# fallback metrics -- exactly the kind of "renders, but wrong" this project
# cannot accept.
cmk libbrotlicommon.a brotli-1.1.0.tar.gz brotli-1.1.0 -DBROTLI_DISABLE_TESTS=ON
# woff2's own woff2_decompress tool links brotlidec without brotlicommon and
# fails; we only need the decode library, which WPE asks for as
# find_package(WOFF2 COMPONENTS dec). Build just those targets.
if [ ! -f "$PREFIX/lib/libwoff2dec.a" ]; then
    echo "==> woff2-1.0.2"
    d=$(unpack woff2-1.0.2.tar.gz woff2-1.0.2)
    TC=$(cmake_toolchain_file)
    b="$WORK/woff2-1.0.2-build"; rm -rf "$b"; mkdir -p "$b"
    cmake -S "$d" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$TC" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DCANONICAL_PREFIXES=ON \
        >"$WORK/woff2-setup.log" 2>&1 || { echo "--- cmake failed ---"; tail -25 "$WORK/woff2-setup.log"; exit 1; }
    cmake --build "$b" -j "$JOBS" --target woff2common woff2dec >"$b/build.log" 2>&1 \
        || { echo "--- build failed ---"; tail -30 "$b/build.log"; exit 1; }
    cp "$b"/libwoff2common.a "$b"/libwoff2dec.a "$PREFIX/lib/" 2>/dev/null
    cp -r "$d"/include/woff2 "$PREFIX/include/" 2>/dev/null
fi

# ---- libwpe: the WPE platform interface ------------------------------------
# SHARED, unlike everything else here, and that is load-bearing. libwpe holds
# process-global state (the loaded backend, the view-backend registry), and
# libWPEWebKit.so hides all its symbols (-fvisibility=hidden, 0 exported wpe_*).
# Linked statically, the browser app would get a SECOND copy of that state and
# its wpe_view_backend would not be the one WebKit is rendering into.
#
# Shared also flips libwpe's loader: meson picks src/loader.c over
# loader-static.c when default_library == shared, so the backend is dlopened at
# runtime (WPE_BACKEND / default-backend) instead of having to resolve
# _wpe_loader_interface at link time.
if [ ! -f "$PREFIX/lib/libwpe-1.0.so" ]; then
    echo "==> libwpe-1.16.0 (shared)"
    d=$(unpack libwpe-1.16.0.tar.xz libwpe-1.16.0)
    CROSS=$(meson_cross_file)
    b="$WORK/libwpe-1.16.0-build"; rm -rf "$b"
    meson setup "$b" "$d" --cross-file "$CROSS" --prefix="$PREFIX" \
        --default-library=shared --buildtype=release -Dprefer_static=true \
        -Dbuild-docs=false -Ddefault-backend=libWPEBackend-n9.so \
        >"$WORK/libwpe-setup.log" 2>&1 || { echo "--- meson setup failed ---"; tail -25 "$WORK/libwpe-setup.log"; exit 1; }
    meson compile -C "$b" -j "$JOBS" >"$b/compile.log" 2>&1 \
        || { echo "--- compile failed ---"; tail -30 "$b/compile.log"; exit 1; }
    meson install -C "$b" --no-rebuild >/dev/null 2>&1
fi

echo
echo "==> WebKit dependency tier installed"
ls "$PREFIX/lib" | grep -E "gcrypt|gpg-error|tasn1|xkbcommon|epoxy|brotli|woff2|wpe|webpdemux" | tr '\n' ' '; echo
