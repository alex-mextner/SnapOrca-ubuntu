#!/usr/bin/env bash
# Headless GUI smoke of a Snapmaker Orca AppImage on the build server (never the laptop: AGENTS.md): checks
# that the 3D canvas is built for GLX (check-gl-backend.sh), starts the extracted AppImage on a private
# Xvfb (GLX, software Mesa llvmpipe) with openbox and a fresh HOME, closes the setup wizard, waits for the main
# window, screenshots the Prepare tab and fails when the 3D canvas region is blank (white or one flat colour):
# the symptom of an EGL canvas on a driver that cannot give it a window surface.
#
#   scripts/ubuntu2604/server-build.sh --name gui-smoke --cmd \
#       'scripts/ubuntu2604/server-gui-smoke.sh ~/work/NAME/build/Snapmaker_Orca_Linux_V2.4.0.AppImage mesa'
#
# runs it under ~/build.lock like every heavy server job (this script takes no lock itself). It needs the
# rootless server environment (server-env.sh install and install-gui run here, idempotently), including the
# classic gdk-pixbuf that replaces the glycin/bubblewrap loaders the server cannot run (CLASSIC_PIXBUF_* there).
#
# Usage: server-gui-smoke.sh APPIMAGE LABEL
# Output (GUI_SMOKE_DIR, default ~/work/gui-smoke, then LABEL/): shots/03-prepare.png (whole screen),
# shots/canvas-crop.png (the checked region), app.log, glxinfo.txt, driver.log.
# Exit status: 0 PASS, 1 FAIL (not GLX, app did not reach the main window, blank canvas), 2 usage / environment.
set -uo pipefail

die() { echo "$0: $2" >&2; exit "$1"; }

[[ $# -eq 2 && -f $1 ]] || die 2 "usage: $0 APPIMAGE LABEL"
APP=$(readlink -f "$1") LABEL=$2
# LABEL names a directory that is wiped below: a single plain path component only.
[[ $LABEL =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die 2 "LABEL must be a plain name (letters, digits, . _ -), got: $LABEL"
# Pixels from the top-left corner of the main window to the "Prepare" tab (Home is the tab the app starts on).
PREPARE_TAB_DX=${PREPARE_TAB_DX:-126} PREPARE_TAB_DY=${PREPARE_TAB_DY:-44}
HERE=$(dirname "$(readlink -f "$0")")

bash "$HERE/server-env.sh" install || die 2 "server-env.sh install failed"
bash "$HERE/server-env.sh" install-gui || die 2 "server-env.sh install-gui failed"
# shellcheck disable=SC1091
source "$HERE/server-env.sh"
server_env_gui_activate || die 2 "server_env_gui_activate failed"

W=${GUI_SMOKE_DIR:-$HOME/work/gui-smoke}/$LABEL
rm -rf "$W"
mkdir -p "$W"/{shots,data,extract}
export W

echo "== $(basename "$APP") sha256 $(sha256sum "$APP" | cut -d' ' -f1)"
(cd "$W/extract" && "$APP" --appimage-extract >/dev/null) || die 2 "cannot extract $APP"
ROOT=$W/extract/squashfs-root
[[ -x $ROOT/AppRun ]] || die 2 "no AppRun in the extracted AppImage"
"$HERE/check-gl-backend.sh" "$ROOT/bin/snapmaker-orca"
rc=$?
case $rc in
    0) ;;
    1) echo "FAIL $LABEL: the AppImage canvas is not built for GLX"; exit 1 ;;
    *) die 2 "check-gl-backend.sh could not check $ROOT/bin/snapmaker-orca" ;;
esac

# A display number nobody uses, so a concurrent Xvfb (a GUI test of another checkout) is left alone.
for n in $(seq 90 99); do
    [[ -e /tmp/.X11-unix/X$n || -e /tmp/.X$n-lock ]] || { GUI_DISPLAY=:$n; break; }
done
[[ -n ${GUI_DISPLAY:-} ]] || die 2 "no free X display between :90 and :99"
export GUI_DISPLAY
# shellcheck disable=SC1091
source "$HERE/gui-test/gui-lib.sh" # DISPLAY, HOME=$W/home (fresh), shot, start_display, wait_win, close_wizard

# kill_tree PID: PID and its descendants, children first (WebKit helper processes outlive a killed parent).
kill_tree() {
    local child
    for child in $(pgrep -P "$1"); do kill_tree "$child"; done
    kill "$1" 2>/dev/null
}
cleanup() {
    shot zz-final
    [[ -z ${RUNNER:-} ]] || kill_tree "$RUNNER"
    # shellcheck disable=SC2046 # the background jobs of this shell: Xvfb and openbox of start_display
    kill $(jobs -p) 2>/dev/null
}
trap cleanup EXIT
fail() {
    log "FAIL: $*"
    echo "FAIL $LABEL: $* (details in $W)"
    exit 1
}

start_display || die 2 "cannot start Xvfb"
# Xvfb exits at once when another X server took the display between the search above and now; the readiness
# probe of start_display would then have talked to that other server.
sleep 1
bg_pids=$(jobs -p)
kill -0 "${bg_pids%%$'\n'*}" 2>/dev/null || die 2 "Xvfb died: display $GUI_DISPLAY is in use (see $W/xvfb.log)"
glxinfo -B 2>&1 | grep -E "OpenGL (vendor|renderer|version)|direct rendering" | tee "$W/glxinfo.txt"

# No 3D-mouse enumeration on the server; the datadir is fresh, so the setup wizard opens first.
cat >"$W/data/Snapmaker_Orca.conf" <<'EOF'
{"app": {"skip_3dmouse_detect": "true", "log_severity_level": "info"}}
EOF
APPDIR=$ROOT "$ROOT/AppRun" --datadir "$W/data" >"$W/app.log" 2>&1 &
RUNNER=$!
log "app=$APP pid=$RUNNER display=$GUI_DISPLAY"

close_wizard 01 240 || fail "no setup wizard window (the app exited or hung; see $W/app.log and $W/data/log)"
wid=$(wait_win 'Untitled' 120) || fail "main window did not appear (see $W/app.log)"
log "main window $wid"
sleep 3 # let the home page settle
eval "$(xdotool getwindowgeometry --shell "$wid" | grep -E '^(X|Y|WIDTH|HEIGHT)=')"
shot 02-home
# The 3D canvas is the right 60% of the Prepare tab, below the tab bar and the tool bar of the 3D view; the crop
# stays inside it (sidebar, tab bar and toolbar are drawn by GTK and would pass for a drawn canvas).
cw=$((WIDTH * 55 / 100)) ch=$((HEIGHT * 70 / 100))
cx=$((X + WIDTH * 40 / 100)) cy=$((Y + HEIGHT * 18 / 100))
crop() { import -window root -crop "${cw}x${ch}+${cx}+${cy}" +repage "$1" 2>/dev/null; }
crop "$W/shots/home-crop.png" || fail "cannot capture the canvas region"
# The app starts on the Home tab: click "Prepare" in the tab bar (offset from the window's top-left corner).
log "main window ${WIDTH}x${HEIGHT}+$X+$Y, clicking the Prepare tab at $((X + PREPARE_TAB_DX)),$((Y + PREPARE_TAB_DY))"
xdotool mousemove --sync $((X + PREPARE_TAB_DX)) $((Y + PREPARE_TAB_DY)) click 1
# Wait for the Prepare tab: the region differs from the Home tab's and stays unchanged between two captures
# (first frames, shaders and the bed texture are in by then).
settled=''
for _ in $(seq 40); do
    sleep 1
    crop "$W/shots/canvas-crop.png" || fail "cannot capture the canvas region"
    if ! cmp -s "$W/shots/canvas-crop.png" "$W/shots/home-crop.png" && [[ -f $W/shots/prev-crop.png ]] &&
        cmp -s "$W/shots/canvas-crop.png" "$W/shots/prev-crop.png"; then
        settled=1
        break
    fi
    cp "$W/shots/canvas-crop.png" "$W/shots/prev-crop.png"
done
[[ -n $settled ]] || { shot 03-prepare; fail "the Prepare tab did not open or its canvas never settled (see shots/02-home.png, shots/03-prepare.png)"; }
# A first frame that is still blank stays unchanged for a moment too: give the GL canvas time before the verdict.
sleep 5
crop "$W/shots/canvas-crop.png" || fail "cannot capture the canvas region"
shot 03-prepare
colors=$(convert "$W/shots/canvas-crop.png" -format %k info: 2>/dev/null)
# Share of (near-)white pixels: a blank canvas is white throughout, a drawn one has the bed and a grey backdrop.
white=$(convert "$W/shots/canvas-crop.png" -colorspace Gray -threshold 98% -format '%[fx:mean]' info: 2>/dev/null)
[[ -n $colors && -n $white ]] || fail "ImageMagick convert could not read the canvas region"
log "canvas ${cw}x${ch}+${cx}+${cy}: $colors distinct colours, white share $white"

if ((colors < 32)) || awk -v w="$white" 'BEGIN { exit !(w > 0.97) }'; then
    fail "the 3D canvas is blank ($colors colours, white share $white)"
fi
echo "PASS $LABEL: 3D canvas drawn ($colors colours, white share $white); screenshot $W/shots/03-prepare.png"
