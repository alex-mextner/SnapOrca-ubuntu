#!/usr/bin/env bash
# Run the INSTALLED Snapmaker Orca AppImage the way the desktop launcher does, on the real GNOME session, without
# ever starting it on the bare laptop (AGENTS.md: its USB/HID enumeration has hung the machine).
#
# What matches the launcher: the file named by the .desktop Exec line, started through its own AppRun from the FUSE mount
# the AppImage runtime makes, with the systemd user environment gnome-shell passes to launched apps (WAYLAND_DISPLAY,
# XDG_SESSION_TYPE, DISPLAY, XAUTHORITY, DBUS_SESSION_BUS_ADDRESS, ...), on the real display (XWayland, mutter scaling,
# the NVIDIA driver), with a copy of the real config (under XDG_CONFIG_HOME of the session, default ~/.config). What differs, on purpose: bubblewrap hides USB/HID (see sandbox.py)
# and the config is a copy.
#
#   launcher-path.sh start [--net] [--project FILE] [--appimage FILE] [--setenv K=V]...
#   launcher-path.sh window          X11 id of the app's main window (waits for it)
#   launcher-path.sh activate        bring that window forward
#   launcher-path.sh shot OUT.png    composited desktop screenshot (capture.py)
#   launcher-path.sh input ACTION... keys / clicks, only while the app window is active (inject.py)
#   launcher-path.sh log             the newest Orca log of the run
#   launcher-path.sh stop            stop the app and unmount, delete the run directory
#
# The mount and the app run as transient systemd user services: a shell with no_new_privs (agent tool shells) cannot
# FUSE-mount through the setuid fusermount3, a service of the user manager can, exactly like the launcher's scope.
# Needs: bwrap, systemd-run, xwininfo, libXRes (window.py), xprop, python3-gi, gst-launch-1.0 (pipewiresrc, pngenc), an unlocked GNOME session.
set -euo pipefail

HERE=$(dirname "$(readlink -f "$0")")
STATE=${XDG_STATE_HOME:-$HOME/.local/state}/snapmaker-orca-launcher-path
DESKTOP=$HOME/.local/share/applications/snapmaker-orca.desktop
MOUNT_UNIT=snapmaker-orca-lp-mount
RUN_UNIT=snapmaker-orca-lp-run

die() { echo "launcher-path.sh: $*" >&2; exit 2; }

# Exec= of the desktop entry, without the field codes (%F, %U, ...): the file the launcher really starts.
launcher_appimage() {
    [[ -f $DESKTOP ]] || die "no desktop entry at $DESKTOP"
    local exec_line
    exec_line=$(sed -n 's/^Exec=//p' "$DESKTOP" | head -n1)
    if [[ $exec_line == \"* ]]; then
        exec_line=${exec_line#\"}
        exec_line=${exec_line%%\"*}
    else
        exec_line=${exec_line%% *}
    fi
    [[ -f $exec_line ]] || die "the desktop entry starts $exec_line, which is not a file"
    readlink -f "$exec_line"
}

stop_all() {
    systemctl --user stop "$RUN_UNIT.service" "$MOUNT_UNIT.service" 2>/dev/null || true
    if [[ -f $STATE/mount ]]; then
        local m
        m=$(<"$STATE/mount")
        # No mountpoint check: a mount whose daemon died is not stat-able and would be skipped, then leak.
        systemd-run --user --pipe --wait --quiet -- fusermount3 -uz "$m" 2>/dev/null || true
        rm -f -- "$STATE/mount"
    fi
    if [[ -f $STATE/run ]]; then
        local r
        r=$(<"$STATE/run")
        [[ $r == "$STATE"/run.* ]] && rm -rf -- "$r"
        rm -f -- "$STATE/run"
    fi
}

cmd_start() {
    local net=() project='' appimage='' setenv=()
    while (($#)); do
        case $1 in
            --net) net=(--net) ;;
            --project) (($# >= 2)) || die "$1 needs a value"; project=$(readlink -f "$2"); shift ;;
            --appimage) (($# >= 2)) || die "$1 needs a value"; appimage=$(readlink -f "$2"); shift ;;
            --setenv)
                (($# >= 2)) || die "$1 needs a value"
                [[ $2 == XDG_CONFIG_HOME=* || $2 == XDG_CACHE_HOME=* ]] && die "$2: XDG_CONFIG_HOME/XDG_CACHE_HOME cannot be overridden"
                setenv+=(--setenv "$2"); shift ;;
            *) die "unknown start option: $1" ;;
        esac
        shift
    done
    [[ -z $project || -f $project ]] || die "no such project: $project"
    [[ -n $appimage ]] || appimage=$(launcher_appimage)
    command -v bwrap >/dev/null || die "bwrap is not installed"
    mkdir -p "$STATE"
    # Two concurrent starts would tear down each other's run (stop_all) and share the unit names.
    exec 9>"$STATE/lock"
    flock -n 9 || die "another start is in progress"
    stop_all

    local run
    run=$(mktemp -d "$STATE/run.XXXXXX")
    echo "$run" >"$STATE/run"
    local config_dir cache_dir
    config_dir=$(python3 "$HERE/session_env.py" config-dir)
    cache_dir=$(python3 "$HERE/session_env.py" cache-dir)
    if [[ -d $config_dir ]]; then
        # -L: a symlinked config directory or child is copied as its content, so the real one is never aliased and written.
        cp -aL "$config_dir" "$run/config"
    else
        mkdir -p "$run/config"
    fi
    # The copy carries the real Orca's logs; `log` must show this run's only.
    rm -rf -- "$run/config/log"
    mkdir "$run/cache"
    local args=()
    if [[ -n $project ]]; then
        # Orca picks the loader by extension, so keep it.
        local name=project
        [[ ${project##*/} == *.* ]] && name=project.${project##*.}
        cp -- "$project" "$run/cache/$name"
        args=("$cache_dir/$name")
    fi

    : >"$STATE/mount.out"
    : >"$STATE/mount.err"
    systemd-run --user --quiet --unit="$MOUNT_UNIT" --collect -p RuntimeMaxSec="${RUNSEC:-1800}" -p StandardOutput="file:$STATE/mount.out" \
        -p StandardError="file:$STATE/mount.err" "$appimage" --appimage-mount
    local mnt=''
    for _ in $(seq 50); do
        mnt=$(head -n1 "$STATE/mount.out")
        [[ -n $mnt ]] && break
        sleep 0.2
    done
    [[ -n $mnt ]] || die "the AppImage did not mount: $(cat "$STATE/mount.err")"
    echo "$mnt" >"$STATE/mount"

    echo "== $(basename "$appimage") sha256 $(sha256sum "$appimage" | cut -d' ' -f1), mounted at $mnt"
    systemd-run --user --quiet --unit="$RUN_UNIT" --collect -p RuntimeMaxSec="${RUNSEC:-1800}" -p KillMode=control-group \
        -p StandardOutput="file:$run/app.out" -p StandardError="file:$run/app.err" \
        "$(command -v python3)" "$HERE/sandbox.py" --appimage "$appimage" "${net[@]}" "${setenv[@]}" "$run" "$mnt" -- "$mnt/AppRun" "${args[@]}"
    echo "== started unit $RUN_UNIT.service, run directory $run"
}

cmd_window() {
    local seconds=${1:-120}
    # By owner (window.py): the user's own Orca window is never returned. Polls inside one process.
    python3 "$HERE/window.py" "$RUN_UNIT.service" "$seconds" || die "no Snapmaker Orca main window after ${seconds}s (see: $0 log)"
}

main() {
    [[ $# -ge 1 ]] || { sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 2; }
    local sub=$1
    shift
    case $sub in
        start) cmd_start "$@" ;;
        window) cmd_window "$@" ;;
        activate)
            local w
            w=$(cmd_window 5) || exit
            python3 "$HERE/activate.py" "$w" ;;
        shot) [[ $# -eq 1 ]] || die "usage: shot OUT.png"; python3 "$HERE/capture.py" "$1" ;;
        input)
            local w
            w=$(cmd_window 5) || exit
            python3 "$HERE/inject.py" "$w" "$@" ;;
        log)
            local f
            [[ -f $STATE/run ]] || die "no run (start one first)"
            f=$(find "$(<"$STATE/run")/config/log" -name '*.log.0' -printf '%T@ %p\n' 2>/dev/null | sort -n | tail -n1 | cut -d' ' -f2-) || f=''
            [[ -n $f ]] || die "no Orca log yet in the run directory"
            echo "== $f"
            cat -- "$f" ;;
        stop) stop_all ;;
        *) die "unknown subcommand: $sub" ;;
    esac
}

main "$@"
