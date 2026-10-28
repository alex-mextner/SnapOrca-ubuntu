#!/usr/bin/env bash
# Static check that a built Snapmaker Orca draws its 3D canvas through GLX, not EGL: an EGL canvas is
# blank on NVIDIA's proprietary driver (doc/developer-reference/How-to-build.md, Ubuntu 26.04). It never
# starts the app (host rules: AGENTS.md), it only lists the dynamic symbols the slicer binary imports.
#
# Usage: check-gl-backend.sh APPIMAGE|ELF   (an AppImage is read from its squashfs, bin/snapmaker-orca)
# Needs: readelf and nm (binutils); unsquashfs (squashfs-tools) for an AppImage.
# Exit status: 0 GLX canvas, 1 EGL canvas or no glX import, 2 usage / missing tool / unreadable input.
set -euo pipefail

die() { echo "$0: $2" >&2; exit "$1"; }

[[ $# -eq 1 && -f $1 ]] || die 2 "usage: $0 APPIMAGE|ELF"
src=$1
for tool in readelf nm; do
    command -v "$tool" >/dev/null || die 2 "$tool not found"
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

elf=$src
# An AppImage (type 2) has "AI\x02" (hex 414902) at byte 8 of its ELF runtime, followed by a squashfs
# that starts right after the ELF's section headers.
if [[ $(dd if="$src" bs=1 skip=8 count=3 2>/dev/null | od -An -tx1 | tr -d ' \n') == 414902 ]]; then
    command -v unsquashfs >/dev/null || die 2 "unsquashfs (squashfs-tools) is needed to read an AppImage"
    offset=$(readelf -h "$src" | awk -F: '
        /Start of section headers/  { split($2, a, " "); start = a[1] }
        /Size of section headers/   { split($2, a, " "); size = a[1] }
        /Number of section headers/ { split($2, a, " "); count = a[1] }
        END { print start + size * count }')
    unsquashfs -o "$offset" -f -d "$work/root" "$src" bin/snapmaker-orca >/dev/null 2>&1 ||
        die 2 "cannot extract bin/snapmaker-orca from $src"
    elf=$work/root/bin/snapmaker-orca
fi

imports=$(nm -D --undefined-only "$elf" | awk '{ print $NF }') || die 2 "cannot read the dynamic symbols of $elf"
glx=$(grep -c '^glX' <<<"$imports" || true)
# The EGL canvas of wxWidgets (X11 and Wayland: wxGLCanvasEGL) and GLEW's EGL loader (GLEW_EGL).
egl_symbols='eglCreatePlatformWindowSurface|eglGetPlatformDisplay|eglGetProcAddress'
egl=$(grep -E "^($egl_symbols)\$" <<<"$imports" | sort -u | paste -sd, - || true)

if [[ $glx -gt 0 && -z $egl ]]; then
    echo "PASS $src: 3D canvas built for GLX ($glx glX imports, no EGL canvas imports)"
    exit 0
fi
echo "FAIL $src: 3D canvas is not built for GLX (glX imports: $glx; EGL canvas imports: ${egl:-none})" >&2
if [[ $glx -eq 0 && -z $egl ]]; then
    echo "  neither glX nor EGL canvas imports: not the slicer binary, or GL is linked another way" >&2
else
    echo "  rebuild wxWidgets with -DwxUSE_GLCANVAS_EGL=OFF and GLEW with -DGLEW_USE_EGL=OFF (deps/)" >&2
fi
exit 1
