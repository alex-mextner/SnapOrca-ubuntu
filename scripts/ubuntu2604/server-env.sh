#!/usr/bin/env bash
# Rootless build/test environment for the build server (Ubuntu 26.04, no sudo): the .deb packages
# of provision.sh (+ Xvfb/openbox/xdotool/wmctrl/imagemagick) are downloaded with `apt-get download`
# and unpacked with `dpkg -x` into $SERVER_ENV (default ~/sysroot); CMake 3.30 comes from the
# Kitware tarball. Dependencies are resolved by `apt-get -s install` against what the host has.
#
#   scripts/ubuntu2604/server-env.sh install   # idempotent; run on the server (server-build.sh does it)
#   source scripts/ubuntu2604/server-env.sh    # PATH, PKG_CONFIG_PATH, CMAKE_PREFIX_PATH, LD_LIBRARY_PATH, ...
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

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    set -euo pipefail
    [[ ${1:-} == install ]] || { echo "usage: server-env.sh install | source server-env.sh" >&2; exit 2; }
    server_env_install
else
    server_env_activate
fi
