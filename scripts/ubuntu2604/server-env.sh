#!/usr/bin/env bash
# Rootless build/test environment for the build server (Ubuntu 26.04, no sudo): the .deb packages
# of provision.sh (+ Xvfb/openbox/xdotool/wmctrl/imagemagick) are downloaded with `apt-get download`
# and unpacked with `dpkg -x` into $SERVER_ENV (default ~/sysroot); CMake 3.30 comes from the
# Kitware tarball. Dependencies are resolved by `apt-get -s install` against what the host has.
#
#   scripts/ubuntu2604/server-env.sh install   # idempotent; run on the server (server-build.sh does it)
#   source scripts/ubuntu2604/server-env.sh    # PATH, PKG_CONFIG_PATH, CMAKE_PREFIX_PATH, LD_LIBRARY_PATH, ...
#   server_env_gui_activate                    # after sourcing: GUI runs (classic gdk-pixbuf, locales, WebKit paths), see below
#
# Re-running `install` after a change of PACKAGES/CMAKE_VERSION refreshes only what differs.
SERVER_ENV=${SERVER_ENV:-$HOME/sysroot}
CMAKE_VERSION=${CMAKE_VERSION:-3.30.8}
CMAKE_DIR=$SERVER_ENV/cmake-$CMAKE_VERSION
# provision.sh's list plus the headless GUI tools.
PACKAGES="autoconf build-essential ccache curl eglexternalplatform-dev extra-cmake-modules file
 gettext git libblosc-dev libcairo2-dev libcurl4-openssl-dev libdbus-1-dev libfuse2t64 libgl1-mesa-dev
 libglew-dev libglu1-mesa-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamerd-3-dev
 libgtk-3-dev libmspack-dev libsecret-1-dev libsoup2.4-dev libspnav-dev libssl-dev libtool libudev-dev
 libunwind-dev libwayland-dev libwebkit2gtk-4.1-dev libxkbcommon-dev m4 ninja-build pkgconf texinfo
 wayland-protocols xz-utils
 xvfb openbox xdotool wmctrl imagemagick x11-utils x11-xserver-utils mesa-utils xauth"

# Classic (non-sandboxed) gdk-pixbuf for headless GUI runs: Ubuntu 26.04's gdk-pixbuf 2.44 has no loaders of its
# own, it decodes every image through glycin, which starts bubblewrap, and the server forbids the unprivileged
# user namespaces bwrap needs ("setting up uid map: Permission denied"), so GTK aborts with "No image loaders are
# configured". The last release with classic loaders (png/jpeg built in) is unpacked next to the sysroot and only
# server_env_gui_activate puts it on the library path, so builds still link against the 26.04 gdk-pixbuf.
CLASSIC_PIXBUF_DEB=${CLASSIC_PIXBUF_DEB:-libgdk-pixbuf-2.0-0_2.42.10+dfsg-3ubuntu3.3_amd64.deb}
CLASSIC_PIXBUF_URL=${CLASSIC_PIXBUF_URL:-https://archive.ubuntu.com/ubuntu/pool/main/g/gdk-pixbuf/$CLASSIC_PIXBUF_DEB}
CLASSIC_PIXBUF_SHA256=${CLASSIC_PIXBUF_SHA256:-de84e0d777d9c9e39148d8491338e6b8c62bc2d066867266b1643a7a725a86ba}
CLASSIC_PIXBUF_DIR=$SERVER_ENV/classic-pixbuf
WEBKIT_PATCHED_DIR=$SERVER_ENV/webkit-patched

server_env_install() (
    set -euo pipefail
    mkdir -p "$SERVER_ENV/debs"
    local stamp="$SERVER_ENV/.packages.sha" want
    want=$(printf '%s' "$PACKAGES" | sha256sum | cut -d' ' -f1)
    if [[ ! -f $CMAKE_DIR/bin/cmake ]]; then
        curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
            | tar -xz -C "$SERVER_ENV" && mv "$SERVER_ENV/cmake-${CMAKE_VERSION}-linux-x86_64" "$CMAKE_DIR"
    fi
    if [[ $(cat "$stamp" 2>/dev/null) != "$want" ]]; then
        # shellcheck disable=SC2086
        mapfile -t pkgs < <(apt-get -s install --no-install-recommends $PACKAGES | awk '/^Inst /{print $2, $3}' | tee "$SERVER_ENV/installed.txt" | cut -d' ' -f1)
        cd "$SERVER_ENV/debs"
        apt-get download "${pkgs[@]}" >/dev/null
        for d in ./*.deb; do dpkg -x "$d" "$SERVER_ENV"; done
        # Relocate: .pc/.cmake/.la files and absolute symlinks point at /usr.
        find "$SERVER_ENV/usr" -name '*.pc' -exec sed -i -E "s#(^|[= ]|-[IL])/usr#\1$SERVER_ENV/usr#g" {} +
        # Autotools/libtool/texinfo scripts and Perl modules hardcode /usr/share and /usr/bin paths.
        grep -rlIE '(^|[^A-Za-z0-9_./-])/usr/(share|bin/(m4|autom4te|autoconf|autoheader|automake|aclocal|autoreconf|libtoolize))' \
            "$SERVER_ENV"/usr/bin "$SERVER_ENV"/usr/share/{autoconf,automake-*,aclocal-*,libtool,texinfo,gettext,perl5} 2>/dev/null \
            | xargs -r sed -i -E "s#(^|[^A-Za-z0-9_./-])/usr/(share|bin/(m4|autom4te|autoconf|autoheader|automake|aclocal|autoreconf|libtoolize))#\1$SERVER_ENV/usr/\2#g" || true
        find "$SERVER_ENV/usr" -type l -lname '/*' | while read -r l; do
            t=$(readlink "$l")
            if [[ -e $SERVER_ENV$t && $SERVER_ENV$t != "$l" ]]; then ln -sfn "$SERVER_ENV$t" "$l"; fi
        done
        # Xvfb runs xkbcomp from a compiled-in "/usr/bin"; point it at "/dev/shm" (same length), where
        # server_env_activate links the unpacked xkbcomp.
        python3 - "$SERVER_ENV/usr/bin/Xvfb" <<'PY'
import sys
p = sys.argv[1]
b = open(p, "rb").read()
open(p, "wb").write(b.replace(b"/usr/bin\x00", b"/dev/shm\x00"))
PY
        # Libraries the host already has were not downloaded, but sysroot .so symlinks and CMake
        # config files (zstdTargets.cmake, ...) resolve inside the sysroot: mirror them there.
        mkdir -p "$SERVER_ENV/usr/lib/x86_64-linux-gnu"
        for f in /usr/lib/x86_64-linux-gnu/lib*.so*; do
            [[ -L $SERVER_ENV$f && ! -e $SERVER_ENV$f ]] && rm "$SERVER_ENV$f" # dangling or self-loop
            [[ -e $SERVER_ENV$f ]] || ln -s "$f" "$SERVER_ENV$f"
        done
        # Symlinks the packages create in postinst (update-alternatives).
        for tool in aclocal automake; do
            latest=$(compgen -G "$SERVER_ENV/usr/bin/$tool-[0-9]*" | sort -V | tail -1 || true)
            [[ -z $latest ]] || ln -sfn "$(basename "$latest")" "$SERVER_ENV/usr/bin/$tool"
        done
        mkdir -p "$SERVER_ENV/shim"
        cat >"$SERVER_ENV/shim/dpkg" <<'SHIM'
#!/usr/bin/env bash
# `dpkg -l PATTERN...` also lists the packages unpacked into the sysroot (build_linux.sh's dependency
# check greps it); everything else goes to the real dpkg.
real=/usr/bin/dpkg
if [[ ${1:-} == -l ]]; then
    "$real" "$@" 2>/dev/null
    list=$(dirname "$0")/../installed.txt
    shift
    while read -r name ver; do
        for pat in "$@"; do
            # shellcheck disable=SC2053
            [[ $name == $pat ]] && printf 'ii  %s  %s  amd64  (sysroot)\n' "$name" "${ver#(}"
        done
    done <"$list"
    exit 0
fi
exec "$real" "$@"
SHIM
        chmod +x "$SERVER_ENV/shim/dpkg"
        # Docs are not needed and texinfo's Perl modules are not usable from the sysroot (GMP's
        # gmp.info step dies): makeinfo just creates the requested output.
        cat >"$SERVER_ENV/shim/makeinfo" <<'SHIM'
#!/usr/bin/env bash
out=""
while (($#)); do
    case $1 in
        -o) out=$2; shift ;;
        --output=*) out=${1#--output=} ;;
        --version) echo "makeinfo (GNU texinfo) 7.2"; exit 0 ;;
    esac
    shift
done
[[ -z $out ]] || : >"$out"
SHIM
        chmod +x "$SERVER_ENV/shim/makeinfo"
        # Host binutils only find GCC's LTO plugin in /usr/lib/bfd-plugins (root-owned): without it ar/ranlib
        # write no symbol index for -flto objects (undefined references at link time). Pass it explicitly.
        plugin=$(echo "$SERVER_ENV"/usr/libexec/gcc/x86_64-linux-gnu/*/liblto_plugin.so)
        for tool in ar ranlib nm; do
            printf '#!/usr/bin/env bash\nexec /usr/bin/%s --plugin %s "$@"\n' "$tool" "$plugin" >"$SERVER_ENV/shim/$tool"
            chmod +x "$SERVER_ENV/shim/$tool"
        done
        rm -f "$SERVER_ENV/debs"/*.deb
        printf '%s' "$want" >"$stamp"
    fi
    echo "server-env: installed in $SERVER_ENV"
    server_env_install_classic_pixbuf
    server_env_install_ldconfig_shim
    server_env_install_locales
    server_env_install_webkit_path
)

# Snapmaker Orca exits at startup ("Switching language failed") when neither the system language nor en_GB is a
# locale the host has, and the server has only C/POSIX: compile en_US and en_GB into $SERVER_ENV/locale
# (localedef is rootless with --no-archive; server_env_gui_activate sets LOCPATH). Idempotent.
server_env_install_locales() (
    set -euo pipefail
    local l
    for l in en_US en_GB; do
        [[ -f $SERVER_ENV/locale/$l.UTF-8/LC_CTYPE ]] && continue
        mkdir -p "$SERVER_ENV/locale"
        # localedef exits 1 on warnings about the locale source (e.g. unknown keywords) but still writes it.
        localedef --no-archive -i "$l" -f UTF-8 "$SERVER_ENV/locale/$l.UTF-8" || [[ -f $SERVER_ENV/locale/$l.UTF-8/LC_CTYPE ]]
    done
)

# WebKitGTK of the sysroot looks for WebKitWebProcess & co. in its compiled-in /usr/lib/x86_64-linux-gnu/webkit2gtk-4.1
# (root-owned, absent on the server) and ignores WEBKIT_EXEC_PATH in a release build, so every web view aborts the
# app ("Unable to spawn a new child process"). A copy of libwebkit2gtk with that prefix replaced by /dev/shm/webkit2gtk-4.1
# (padded with slashes to the same length) goes to $WEBKIT_PATCHED_DIR; server_env_gui_activate links that path to the
# sysroot's directory, the same way xkbcomp is reached for Xvfb. Idempotent.
server_env_install_webkit_path() (
    set -euo pipefail
    local src dst
    src=$(readlink -f "$SERVER_ENV/usr/lib/x86_64-linux-gnu/libwebkit2gtk-4.1.so.0")
    dst=$WEBKIT_PATCHED_DIR/libwebkit2gtk-4.1.so.0
    [[ $dst -nt $src ]] && return 0
    mkdir -p "$WEBKIT_PATCHED_DIR"
    python3 - "$src" "$dst" <<'PY'
import sys
src, dst = sys.argv[1:]
old = b"/usr/lib/x86_64-linux-gnu/webkit2gtk-4.1"
new = b"/dev/shm/webkit2gtk-4.1".ljust(len(old), b"/")
assert len(new) == len(old)  # the patch must not move anything in the library
data = open(src, "rb").read()
if data.count(old) != 2:
    sys.exit(f"server-env: {src} has {data.count(old)} occurrences of {old.decode()}, expected 2")
open(dst + ".tmp", "wb").write(data.replace(old, new))
PY
    mv "$dst.tmp" "$dst"
)

# The AppImage's libexec/snapmaker-orca-env refuses to start unless `ldconfig -p` (or /usr/lib*) lists
# libOpenGL.so.0 and the WebKitGTK 4.1 libraries, which only exist in the sysroot here: the shim makes
# `ldconfig -p` also list every library of the sysroot (the grep there needs a leading space, not a tab).
# Regenerated on every install, so it follows the sysroot's location.
server_env_install_ldconfig_shim() (
    set -euo pipefail
    mkdir -p "$SERVER_ENV/shim"
    cat >"$SERVER_ENV/shim/ldconfig" <<SHIM
#!/usr/bin/env bash
real=/usr/sbin/ldconfig
if [[ \${1:-} == -p ]]; then
    "\$real" -p
    for f in "$SERVER_ENV"/usr/lib/x86_64-linux-gnu/lib*.so.*; do
        printf '  %s (libc6,x86-64) => %s\n' "\${f##*/}" "\$f"
    done
    exit 0
fi
exec "\$real" "\$@"
SHIM
    chmod +x "$SERVER_ENV/shim/ldconfig"
)

# Unpacks the classic gdk-pixbuf (see CLASSIC_PIXBUF_*) and writes its loaders.cache; idempotent.
server_env_install_classic_pixbuf() (
    set -euo pipefail
    local lib=$CLASSIC_PIXBUF_DIR/usr/lib/x86_64-linux-gnu sysroot_loaders
    [[ -f $CLASSIC_PIXBUF_DIR/loaders.cache && $(cat "$CLASSIC_PIXBUF_DIR/.stamp" 2>/dev/null) == "$CLASSIC_PIXBUF_SHA256 $SERVER_ENV" ]] && return 0
    rm -rf "$CLASSIC_PIXBUF_DIR"
    mkdir -p "$CLASSIC_PIXBUF_DIR"
    curl -fsSL -o "$CLASSIC_PIXBUF_DIR/pixbuf.deb" "$CLASSIC_PIXBUF_URL"
    echo "$CLASSIC_PIXBUF_SHA256  $CLASSIC_PIXBUF_DIR/pixbuf.deb" | sha256sum -c --quiet -
    dpkg -x "$CLASSIC_PIXBUF_DIR/pixbuf.deb" "$CLASSIC_PIXBUF_DIR"
    rm "$CLASSIC_PIXBUF_DIR/pixbuf.deb"
    # The sysroot's librsvg loader (SVG icons) is built against the 26.04 gdk-pixbuf and works with the classic
    # one too; it is listed in the cache when it loads.
    sysroot_loaders=$SERVER_ENV/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders
    LD_LIBRARY_PATH="$lib:$SERVER_ENV/usr/lib/x86_64-linux-gnu:$SERVER_ENV/usr/lib" \
        GDK_PIXBUF_MODULEDIR="$lib/gdk-pixbuf-2.0/2.10.0/loaders" \
        "$lib/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders" "$lib"/gdk-pixbuf-2.0/2.10.0/loaders/*.so "$sysroot_loaders"/libpixbufloader_svg.so \
        >"$CLASSIC_PIXBUF_DIR/loaders.cache" 2>"$CLASSIC_PIXBUF_DIR/query-loaders.log" || true
    # The cache holds absolute paths and no entry means GTK finds no image loaders: fail now, not at run time.
    grep -q libpixbufloader-bmp "$CLASSIC_PIXBUF_DIR/loaders.cache" || {
        echo "server-env: classic gdk-pixbuf loaders.cache lists no loaders (see $CLASSIC_PIXBUF_DIR/query-loaders.log)" >&2
        return 1
    }
    printf '%s' "$CLASSIC_PIXBUF_SHA256 $SERVER_ENV" >"$CLASSIC_PIXBUF_DIR/.stamp"
    echo "server-env: classic gdk-pixbuf installed in $CLASSIC_PIXBUF_DIR"
)

server_env_activate() {
    local p=$SERVER_ENV multi=x86_64-linux-gnu
    export PATH="$p/shim:$CMAKE_DIR/bin:$p/usr/bin:$p/usr/sbin:$PATH"
    export LD_LIBRARY_PATH="$p/usr/lib/$multi:$p/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LIBRARY_PATH="$p/usr/lib/$multi:$p/usr/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
    # -isystem semantics (not CPATH, which acts like -I and would shadow a project's own headers such as the
    # prefixed libpng ones, as /usr/include never does).
    unset CPATH
    export C_INCLUDE_PATH="$p/usr/include:$p/usr/include/$multi${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"
    export CPLUS_INCLUDE_PATH="$C_INCLUDE_PATH"
    export PKG_CONFIG_PATH="$p/usr/lib/$multi/pkgconfig:$p/usr/lib/pkgconfig:$p/usr/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export CMAKE_PREFIX_PATH="$p/usr${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    export ACLOCAL_PATH="$p/usr/share/aclocal${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
    export GCC_EXEC_PREFIX="$p/usr/lib/gcc/"
    export XDG_DATA_DIRS="$p/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
    export XKB_CONFIG_ROOT="$p/usr/share/X11/xkb"
    magick_dirs="$(echo "$p"/etc/ImageMagick-*):$(echo "$p"/usr/lib/$multi/ImageMagick-*/config-Q16HDRI)"
    export MAGICK_CONFIGURE_PATH="$magick_dirs"
    export FONTCONFIG_PATH="$p/etc/fonts" XDG_CONFIG_DIRS="$p/etc/xdg:/etc/xdg"
    export LIBGL_DRIVERS_PATH="$p/usr/lib/$multi/dri"
    ln -sf "$p/usr/bin/xkbcomp" /dev/shm/xkbcomp
    export GIT_EXEC_PATH="$p/usr/lib/git-core" GIT_TEMPLATE_DIR="$p/usr/share/git-core/templates"
    export LC_ALL=C.UTF-8
    export M4="$p/usr/bin/m4" CMAKE_GENERATOR=Ninja
}

# Environment for running the GTK app headless on the server (after server_env_activate): the classic gdk-pixbuf and
# the patched libwebkit2gtk first on the library path, en_US/en_GB locales, WebKit without bubblewrap (the server
# forbids its user namespaces) and without GPU compositing, Mesa as the EGL vendor (WebKit's web process aborts with
# "Could not create default EGL display" without it) and the GSettings schemas of the sysroot.
server_env_gui_activate() {
    local lib=$CLASSIC_PIXBUF_DIR/usr/lib/x86_64-linux-gnu
    export LD_LIBRARY_PATH="$WEBKIT_PATCHED_DIR:$lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export GDK_PIXBUF_MODULEDIR="$lib/gdk-pixbuf-2.0/2.10.0/loaders" GDK_PIXBUF_MODULE_FILE="$CLASSIC_PIXBUF_DIR/loaders.cache"
    # /dev/shm is world-writable and sticky: a link somebody else made there cannot be replaced, and must not be followed.
    ln -sfn "$SERVER_ENV/usr/lib/x86_64-linux-gnu/webkit2gtk-4.1" /dev/shm/webkit2gtk-4.1
    if [[ $(readlink /dev/shm/webkit2gtk-4.1) != "$SERVER_ENV/usr/lib/x86_64-linux-gnu/webkit2gtk-4.1" ]]; then
        echo "server-env: /dev/shm/webkit2gtk-4.1 does not point into $SERVER_ENV (another user's sysroot?)" >&2
        return 1
    fi
    export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 WEBKIT_DISABLE_DMABUF_RENDERER=1
    export __EGL_VENDOR_LIBRARY_FILENAMES="$SERVER_ENV/usr/share/glvnd/egl_vendor.d/50_mesa.json"
    export LOCPATH="$SERVER_ENV/locale"
    export GSETTINGS_SCHEMA_DIR="$SERVER_ENV/usr/share/glib-2.0/schemas"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    set -euo pipefail
    [[ ${1:-} == install ]] || { echo "usage: server-env.sh install | source server-env.sh" >&2; exit 2; }
    server_env_install
else
    server_env_activate
fi
